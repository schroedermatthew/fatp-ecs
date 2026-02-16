#pragma once

/**
 * @file EventBus.h
 * @brief Signal-based event system for the FAT-P ECS framework.
 *
 * @details
 * Provides typed event signals for entity and component lifecycle:
 * - onEntityCreated:      fired when a new entity is created
 * - onEntityDestroyed:    fired when an entity is destroyed
 * - onComponentAdded<T>:  fired when component T is attached to an entity
 * - onComponentRemoved<T>: fired when component T is removed from an entity
 *
 * Events use the fat_p::Signal system, which provides:
 * - Zero heap allocation for <= 4 listeners (SmallVector<Slot, 4>)
 * - RAII ScopedConnection for automatic lifetime management
 * - Reentrancy safety (safe to connect/disconnect during emission)
 * - Priority-based slot ordering
 *
 * Typed component events are stored in a FastHashMap keyed by TypeId,
 * using the same pattern as component stores.
 *
 * FAT-P components used:
 * - Signal: Observer pattern with SmallVector-backed slot storage
 * - FastHashMap: Type-erased storage for per-component-type signals
 * - ScopedConnection: RAII connection lifetime (from Signal.h)
 *
 * @note Thread Safety: Signals are single-threaded by default.
 *       For thread-safe signals, use ThreadSafeSignal aliases.
 */

#include <memory>

#include <fat_p/FastHashMap.h>
#include <fat_p/Signal.h>

#include "Entity.h"

namespace fatp_ecs
{

// Forward declaration
using TypeId = std::size_t;

template <typename T>
TypeId typeId() noexcept;

// =============================================================================
// Type-Erased Signal Interface
// =============================================================================

/**
 * @brief Abstract base for type-erased component event signals.
 *
 * @details
 * Just like IComponentStore provides type erasure for storage,
 * IComponentSignalPair provides type erasure for the pair of
 * added/removed signals for a single component type.
 */
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
 * @brief Concrete typed signal pair for component type T.
 *
 * @tparam T The component type.
 *
 * @details
 * Holds two signals:
 * - onAdded:   emitted with (Entity, T&) when component is attached
 * - onRemoved: emitted with (Entity) when component is detached
 *
 * The onAdded signal passes a mutable reference so listeners can
 * inspect or modify the component immediately after attachment.
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

/**
 * @brief Central event hub for ECS lifecycle events.
 *
 * @details
 * Owned by the Registry and provides both entity-level and
 * component-level event signals.
 *
 * Usage:
 * @code
 * Registry registry;
 *
 * // Entity events
 * auto conn1 = registry.events().onEntityCreated.connect(
 *     [](Entity e) { std::cout << "Created!\n"; });
 *
 * // Component events
 * auto conn2 = registry.events().onComponentAdded<Position>().connect(
 *     [](Entity e, Position& p) { std::cout << "Position added\n"; });
 *
 * Entity e = registry.create();  // fires onEntityCreated
 * registry.add<Position>(e, 10.0f, 20.0f);  // fires onComponentAdded<Position>
 * registry.destroy(e);  // fires onComponentRemoved + onEntityDestroyed
 * @endcode
 */
class EventBus
{
public:
    EventBus() = default;
    ~EventBus() = default;

    // Non-copyable
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Movable
    EventBus(EventBus&&) noexcept = default;
    EventBus& operator=(EventBus&&) noexcept = default;

    // =========================================================================
    // Entity Lifecycle Signals
    // =========================================================================

    /// @brief Fired when a new entity is created. Signature: void(Entity).
    fat_p::Signal<void(Entity)> onEntityCreated;

    /// @brief Fired when an entity is about to be destroyed. Signature: void(Entity).
    fat_p::Signal<void(Entity)> onEntityDestroyed;

    // =========================================================================
    // Component Event Access
    // =========================================================================

    /**
     * @brief Returns the onComponentAdded signal for type T.
     *
     * @tparam T The component type.
     * @return Reference to the signal. Lazily creates the signal pair if needed.
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
     * @return Reference to the signal. Lazily creates the signal pair if needed.
     */
    template <typename T>
    fat_p::Signal<void(Entity)>& onComponentRemoved()
    {
        return ensureSignalPair<T>()->onRemoved;
    }

    // =========================================================================
    // Internal: Emit Helpers (called by Registry)
    // =========================================================================

    /**
     * @brief Emit the component-added event for type T.
     *
     * @details Only emits if a signal pair exists for this type.
     *          Avoids creating signal infrastructure for types nobody listens to.
     */
    template <typename T>
    void emitComponentAdded(Entity entity, T& component)
    {
        auto* pair = getSignalPair<T>();
        if (pair != nullptr)
        {
            pair->onAdded.emit(entity, component);
        }
    }

    /**
     * @brief Emit the component-removed event for type T.
     */
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
    /**
     * @brief Ensures a ComponentSignalPair<T> exists and returns a pointer.
     */
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

    /**
     * @brief Returns the ComponentSignalPair<T> if it exists, nullptr otherwise.
     */
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

    /// @brief Type-erased signal pairs, keyed by TypeId.
    fat_p::FastHashMap<TypeId, std::unique_ptr<IComponentSignalPair>> mSignals;
};

} // namespace fatp_ecs
