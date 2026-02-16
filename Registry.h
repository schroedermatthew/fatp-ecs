#pragma once

/**
 * @file Registry.h
 * @brief Central entity-component registry for the FAT-P ECS framework.
 *
 * @details
 * Registry owns all entity lifecycle and component storage.
 * It is the primary user-facing API for the ECS framework.
 *
 * Entities are allocated via a SlotMap, which provides:
 * - O(1) creation and destruction
 * - Generational handles (ABA safety for stale entity references)
 * - Dense storage for efficient iteration
 *
 * Component stores are kept in a FastHashMap keyed by type ID.
 * Each component type T gets its own ComponentStore<T>, which wraps
 * a SparseSetWithData<uint32_t, T>.
 *
 * FAT-P components used:
 * - SlotMap: Entity allocator with generational safety
 * - FastHashMap: Type-erased component store registry
 * - SparseSetWithData: Per-component-type storage (via ComponentStore<T>)
 * - StrongId: Type-safe Entity handles (via Entity.h)
 * - SmallVector: Internal buffers for entity queries
 *
 * @note Thread Safety: NOT thread-safe. External synchronization required
 *       for concurrent access. Phase 2 will add ThreadPool-based parallel
 *       iteration.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <typeindex>
#include <typeinfo>

#include <fat_p/FastHashMap.h>
#include <fat_p/SlotMap.h>
#include <fat_p/SmallVector.h>

#include "ComponentStore.h"
#include "Entity.h"
#include "View.h"

namespace fatp_ecs
{

// =============================================================================
// TypeId Helper
// =============================================================================

/**
 * @brief Generates a unique runtime type identifier for component types.
 *
 * @details
 * Uses a static local counter pattern — each unique T gets a monotonically
 * increasing ID the first time typeId<T>() is called. This is faster than
 * std::type_index for hash map lookups (integer hash vs pointer hash).
 *
 * @note Thread Safety: The ID assignment is NOT thread-safe. In a
 *       multi-threaded init scenario, call typeId<T>() for all component
 *       types during single-threaded startup.
 */
using TypeId = std::size_t;

namespace detail
{

inline TypeId nextTypeId() noexcept
{
    static TypeId counter = 0;
    return counter++;
}

} // namespace detail

/**
 * @brief Returns a unique TypeId for component type T.
 *
 * @tparam T The component type.
 * @return A unique TypeId that is stable for the lifetime of the program.
 */
template <typename T>
TypeId typeId() noexcept
{
    static const TypeId id = detail::nextTypeId();
    return id;
}

// =============================================================================
// EntityMetadata
// =============================================================================

/**
 * @brief Metadata stored per entity in the SlotMap.
 *
 * @details
 * Currently minimal — just tracks whether the entity is alive.
 * Future phases may add component bitmask (BitSet), name (StringPool),
 * or hierarchy information here.
 */
struct EntityMetadata
{
    bool alive = true;
};

// =============================================================================
// Registry
// =============================================================================

/**
 * @brief Central entity-component registry.
 *
 * @details
 * The Registry is the main entry point for the ECS framework.
 * All entity creation, destruction, component attachment, and
 * component querying goes through this class.
 *
 * Example usage:
 * @code
 * Registry registry;
 *
 * Entity e = registry.create();
 * registry.add<Position>(e, 10.0f, 20.0f);
 * registry.add<Velocity>(e, 1.0f, -0.5f);
 *
 * auto view = registry.view<Position, Velocity>();
 * view.each([](Entity entity, Position& pos, Velocity& vel) {
 *     pos.x += vel.dx;
 *     pos.y += vel.dy;
 * });
 *
 * registry.destroy(e);
 * @endcode
 */
class Registry
{
public:
    /// @brief Handle type from the entity SlotMap.
    using EntityHandle = fat_p::SlotMapHandle;

    Registry() = default;
    ~Registry() = default;

    // Non-copyable (owns unique_ptrs and SlotMap state)
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Movable
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;

    // =========================================================================
    // Entity Lifecycle
    // =========================================================================

    /**
     * @brief Creates a new entity and returns its ID.
     *
     * @return A valid Entity that can have components attached.
     *
     * @details
     * The entity is allocated from the SlotMap, which may reuse
     * a previously-destroyed slot with an incremented generation.
     * The returned Entity encodes the slot index; the generation
     * is tracked internally for validity checks.
     *
     * @note Complexity: O(1) amortized.
     */
    [[nodiscard]] Entity create()
    {
        EntityHandle handle = mEntities.insert(EntityMetadata{true});

        // Pack both index and generation into the Entity.
        // This ensures that even if a slot is reused, the old Entity
        // value will differ from the new one (different generation).
        return EntityTraits::make(handle.index, handle.generation);
    }

    /**
     * @brief Destroys an entity and removes all its components.
     *
     * @param entity The entity to destroy.
     * @return true if the entity was destroyed; false if it was already
     *         dead or invalid.
     *
     * @details
     * Removes the entity from every component store it belongs to,
     * then erases it from the SlotMap. After this call, the Entity
     * value is stale — isAlive() will return false, and component
     * access will fail safely.
     *
     * @note Complexity: O(k) where k is the number of registered
     *       component types (probes each store's contains()).
     */
    bool destroy(Entity entity)
    {
        if (!isAlive(entity))
        {
            return false;
        }

        // Remove from all component stores
        for (auto it = mStores.begin(); it != mStores.end(); ++it)
        {
            it.value()->remove(entity);
        }

        // Erase from SlotMap using the embedded handle
        EntityHandle handle{EntityTraits::index(entity), EntityTraits::generation(entity)};
        mEntities.erase(handle);

        return true;
    }

    /**
     * @brief Returns true if entity is alive (created but not destroyed).
     *
     * @param entity The entity to check.
     * @return true if alive; false if destroyed, invalid, or never created.
     *
     * @note Complexity: O(1).
     */
    [[nodiscard]] bool isAlive(Entity entity) const noexcept
    {
        if (entity == NullEntity)
        {
            return false;
        }

        EntityHandle handle{EntityTraits::index(entity), EntityTraits::generation(entity)};
        return mEntities.get(handle) != nullptr;
    }

    /**
     * @brief Returns the number of alive entities.
     *
     * @note Complexity: O(1).
     */
    [[nodiscard]] std::size_t entityCount() const noexcept
    {
        return mEntities.size();
    }

    // =========================================================================
    // Component Operations
    // =========================================================================

    /**
     * @brief Adds a component of type T to entity, constructed in-place.
     *
     * @tparam T The component type.
     * @tparam Args Constructor argument types for T.
     * @param entity The entity to add the component to.
     * @param args Arguments forwarded to T's constructor.
     * @return Reference to the newly added component.
     *
     * @throws std::invalid_argument if entity is not alive.
     * @throws (none additional) if entity already has T — the existing
     *         component is returned unchanged (idempotent).
     *
     * @note Complexity: O(1) amortized.
     */
    template <typename T, typename... Args>
    T& add(Entity entity, Args&&... args)
    {
        auto* store = ensureStore<T>();

        // If already has component, return existing
        T* existing = store->tryGet(entity);
        if (existing != nullptr)
        {
            return *existing;
        }

        store->emplace(entity, std::forward<Args>(args)...);
        return *store->tryGet(entity);
    }

    /**
     * @brief Removes component T from entity.
     *
     * @tparam T The component type.
     * @param entity The entity to remove the component from.
     * @return true if the component was removed; false if entity didn't have T.
     *
     * @note Complexity: O(1).
     */
    template <typename T>
    bool remove(Entity entity)
    {
        auto* store = getStore<T>();
        if (store == nullptr)
        {
            return false;
        }
        return store->remove(entity);
    }

    /**
     * @brief Returns true if entity has component T.
     *
     * @tparam T The component type.
     * @param entity The entity to check.
     * @return true if entity has T; false otherwise.
     *
     * @note Complexity: O(1).
     */
    template <typename T>
    [[nodiscard]] bool has(Entity entity) const
    {
        const auto* store = getStore<T>();
        if (store == nullptr)
        {
            return false;
        }
        return store->has(entity);
    }

    /**
     * @brief Returns a reference to entity's component T.
     *
     * @tparam T The component type.
     * @param entity The entity to get the component for.
     * @return Reference to the component.
     * @throws std::out_of_range if entity does not have T.
     *
     * @note Complexity: O(1).
     */
    template <typename T>
    [[nodiscard]] T& get(Entity entity)
    {
        auto* store = getStore<T>();
        if (store == nullptr)
        {
            throw std::out_of_range("Registry::get: component type not registered");
        }
        return store->get(entity);
    }

    template <typename T>
    [[nodiscard]] const T& get(Entity entity) const
    {
        const auto* store = getStore<T>();
        if (store == nullptr)
        {
            throw std::out_of_range("Registry::get: component type not registered");
        }
        return store->get(entity);
    }

    /**
     * @brief Returns a pointer to entity's component T, or nullptr.
     *
     * @tparam T The component type.
     * @param entity The entity to look up.
     * @return Pointer to component, or nullptr if absent.
     *
     * @note Complexity: O(1).
     */
    template <typename T>
    [[nodiscard]] T* tryGet(Entity entity)
    {
        auto* store = getStore<T>();
        if (store == nullptr)
        {
            return nullptr;
        }
        return store->tryGet(entity);
    }

    template <typename T>
    [[nodiscard]] const T* tryGet(Entity entity) const
    {
        const auto* store = getStore<T>();
        if (store == nullptr)
        {
            return nullptr;
        }
        return store->tryGet(entity);
    }

    // =========================================================================
    // Views
    // =========================================================================

    /**
     * @brief Creates a view for iterating entities with all components Ts.
     *
     * @tparam Ts Component types required.
     * @return A View<Ts...> that can be iterated with each().
     *
     * @details
     * The view is lightweight (just pointers to stores). Creating it
     * is O(k) where k = sizeof...(Ts). Iterating is O(n * k) where
     * n is the size of the smallest store.
     *
     * If any component type has never been registered (no entity has
     * ever had that component), the view will match zero entities.
     */
    template <typename... Ts>
    [[nodiscard]] View<Ts...> view()
    {
        return View<Ts...>(getOrNullStore<Ts>()...);
    }

    // =========================================================================
    // Bulk Operations
    // =========================================================================

    /**
     * @brief Destroys all entities and clears all component stores.
     *
     * @details
     * After this call, entityCount() == 0 and all component stores
     * are empty. The SlotMap's generation counters are incremented
     * to invalidate all outstanding Entity handles.
     */
    void clear()
    {
        for (auto it = mStores.begin(); it != mStores.end(); ++it)
        {
            it.value()->clear();
        }
        mEntities.clear();
    }

    /**
     * @brief Returns a SmallVector of all alive entities.
     *
     * @details
     * Uses SmallVector<Entity, 64> — queries returning <= 64 entities
     * avoid heap allocation entirely (the common case for targeted
     * entity queries in gameplay code).
     *
     * @note Complexity: O(n) where n is the number of alive entities.
     */
    [[nodiscard]] fat_p::SmallVector<Entity, 64> allEntities() const
    {
        fat_p::SmallVector<Entity, 64> result;
        for (const auto& entry : mEntities.entries())
        {
            result.push_back(EntityTraits::make(entry.handle.index, entry.handle.generation));
        }
        return result;
    }

private:
    // =========================================================================
    // Internal: Store Management
    // =========================================================================

    /**
     * @brief Ensures a ComponentStore<T> exists and returns a pointer to it.
     *
     * Creates the store if it doesn't exist yet.
     */
    template <typename T>
    ComponentStore<T>* ensureStore()
    {
        const TypeId tid = typeId<T>();

        auto* existing = mStores.find(tid);
        if (existing != nullptr)
        {
            return static_cast<ComponentStore<T>*>(existing->get());
        }

        auto store = std::make_unique<ComponentStore<T>>();
        auto* raw = store.get();
        mStores.insert(tid, std::move(store));
        return raw;
    }

    /**
     * @brief Returns a pointer to the ComponentStore<T>, or nullptr.
     */
    template <typename T>
    ComponentStore<T>* getStore()
    {
        const TypeId tid = typeId<T>();
        auto* val = mStores.find(tid);
        if (val == nullptr)
        {
            return nullptr;
        }
        return static_cast<ComponentStore<T>*>(val->get());
    }

    template <typename T>
    const ComponentStore<T>* getStore() const
    {
        const TypeId tid = typeId<T>();
        const auto* val = mStores.find(tid);
        if (val == nullptr)
        {
            return nullptr;
        }
        return static_cast<const ComponentStore<T>*>(val->get());
    }

    /**
     * @brief Returns the store for T if it exists, nullptr otherwise.
     * Used by view() — a missing store means zero matches.
     */
    template <typename T>
    ComponentStore<T>* getOrNullStore()
    {
        return getStore<T>();
    }

    // =========================================================================
    // Data Members
    // =========================================================================

    /// Entity allocator with generational safety.
    fat_p::SlotMap<EntityMetadata> mEntities;

    /// Type-erased component stores, keyed by TypeId.
    fat_p::FastHashMap<TypeId, std::unique_ptr<IComponentStore>> mStores;
};

} // namespace fatp_ecs
