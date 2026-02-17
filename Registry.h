#pragma once

/**
 * @file Registry.h
 * @brief Central entity-component registry for the FAT-P ECS framework.
 */

// FAT-P components used:
// - SlotMap: Entity allocator with generational safety
// - FastHashMap: Type-erased component store registry
// - SparseSetWithData: Per-component-type storage (via ComponentStore<T>)
// - StrongId: Type-safe Entity handles (via Entity.h)
// - SmallVector: Stack-allocated entity query results
// - Signal: Event system (via EventBus)
// - BitSet: Component masks (via ComponentMask.h)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <typeindex>
#include <typeinfo>

#include <fat_p/FastHashMap.h>
#include <fat_p/SlotMap.h>
#include <fat_p/SmallVector.h>

#include "ComponentMask.h"
#include "ComponentStore.h"
#include "Entity.h"
#include "EventBus.h"
#include "View.h"

namespace fatp_ecs
{

// =============================================================================
// TypeId Helper
// =============================================================================

using TypeId = std::size_t;

namespace detail
{

inline TypeId nextTypeId() noexcept
{
    static TypeId counter = 0;
    return counter++;
}

} // namespace detail

template <typename T>
TypeId typeId() noexcept
{
    static const TypeId id = detail::nextTypeId();
    return id;
}

/// @brief Builds a ComponentMask from a list of component types.
template <typename... Ts>
ComponentMask makeComponentMask()
{
    ComponentMask mask;
    (mask.set(typeId<Ts>()), ...);
    return mask;
}

// =============================================================================
// EntityMetadata
// =============================================================================

/// @brief Per-entity data stored in the SlotMap.
struct EntityMetadata
{
    bool alive = true;
    ComponentMask mask;
};

// =============================================================================
// Registry
// =============================================================================

/// @brief Central coordinator for entity lifecycle and component storage.
/// @note Thread-safety: NOT thread-safe. Use Scheduler for parallel access.
class Registry
{
public:
    using EntityHandle = fat_p::SlotMapHandle;

    Registry() = default;
    ~Registry() = default;

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;

    // =========================================================================
    // Event System Access
    // =========================================================================

    [[nodiscard]] EventBus& events() noexcept { return mEvents; }
    [[nodiscard]] const EventBus& events() const noexcept { return mEvents; }

    // =========================================================================
    // Entity Lifecycle
    // =========================================================================

    [[nodiscard]] Entity create()
    {
        EntityHandle handle = mEntities.insert(EntityMetadata{true, {}});
        Entity entity = EntityTraits::make(handle.index, handle.generation);
        mEvents.onEntityCreated.emit(entity);
        return entity;
    }

    bool destroy(Entity entity)
    {
        if (!isAlive(entity))
        {
            return false;
        }

        // Fire destruction event before removing anything
        mEvents.onEntityDestroyed.emit(entity);

        for (auto it = mStores.begin(); it != mStores.end(); ++it)
        {
            it.value()->remove(entity);
        }

        EntityHandle handle{EntityTraits::index(entity),
                            EntityTraits::generation(entity)};
        mEntities.erase(handle);
        return true;
    }

    [[nodiscard]] bool isAlive(Entity entity) const noexcept
    {
        if (entity == NullEntity)
        {
            return false;
        }

        EntityHandle handle{EntityTraits::index(entity),
                            EntityTraits::generation(entity)};
        return mEntities.get(handle) != nullptr;
    }

    [[nodiscard]] std::size_t entityCount() const noexcept
    {
        return mEntities.size();
    }

    // =========================================================================
    // Component Operations
    // =========================================================================

    template <typename T, typename... Args>
    T& add(Entity entity, Args&&... args)
    {
        auto* store = ensureStore<T>();

        T* existing = store->tryGet(entity);
        if (existing != nullptr)
        {
            return *existing;
        }

        T* inserted = store->emplace(entity, std::forward<Args>(args)...);

        updateMask(entity, typeId<T>(), true);
        mEvents.emitComponentAdded<T>(entity, *inserted);

        return *inserted;
    }

    template <typename T>
    bool remove(Entity entity)
    {
        auto* store = getStore<T>();
        if (store == nullptr)
        {
            return false;
        }

        if (!store->has(entity))
        {
            return false;
        }

        mEvents.emitComponentRemoved<T>(entity);
        updateMask(entity, typeId<T>(), false);

        return store->remove(entity);
    }

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
    // Component Mask Queries
    // =========================================================================

    [[nodiscard]] ComponentMask mask(Entity entity) const noexcept
    {
        EntityHandle handle{EntityTraits::index(entity),
                            EntityTraits::generation(entity)};
        const auto* meta = mEntities.get(handle);
        if (meta == nullptr)
        {
            return {};
        }
        return meta->mask;
    }

    // =========================================================================
    // Views
    // =========================================================================

    template <typename... Ts>
    [[nodiscard]] View<Ts...> view()
    {
        return View<Ts...>(getOrNullStore<Ts>()...);
    }

    // =========================================================================
    // Bulk Operations
    // =========================================================================

    void clear()
    {
        for (auto it = mStores.begin(); it != mStores.end(); ++it)
        {
            it.value()->clear();
        }
        mEntities.clear();
    }

    [[nodiscard]] fat_p::SmallVector<Entity, 64> allEntities() const
    {
        fat_p::SmallVector<Entity, 64> result;
        for (const auto& entry : mEntities.entries())
        {
            result.push_back(
                EntityTraits::make(entry.handle.index, entry.handle.generation));
        }
        return result;
    }

private:
    // =========================================================================
    // Internal: Store Management
    // =========================================================================

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

    template <typename T>
    ComponentStore<T>* getOrNullStore()
    {
        return getStore<T>();
    }

    // =========================================================================
    // Internal: Mask Management
    // =========================================================================

    void updateMask(Entity entity, TypeId tid, bool set)
    {
        EntityHandle handle{EntityTraits::index(entity),
                            EntityTraits::generation(entity)};
        auto* meta = mEntities.get(handle);
        if (meta != nullptr && tid < kMaxComponentTypes)
        {
            if (set)
            {
                meta->mask.set(tid);
            }
            else
            {
                meta->mask.clear(tid);
            }
        }
    }

    // =========================================================================
    // Data Members
    // =========================================================================

    fat_p::SlotMap<EntityMetadata> mEntities;
    fat_p::FastHashMap<TypeId, std::unique_ptr<IComponentStore>> mStores;
    EventBus mEvents;
};

} // namespace fatp_ecs
