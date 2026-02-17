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

#include <array>
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
#include "TypeId.h"
#include "View.h"

namespace fatp_ecs
{

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
        EntityHandle handle = mEntities.insert_fast(uint8_t{0});
        Entity entity = EntityTraits::make(handle.index, handle.generation);
        if (mEvents.onEntityCreated.slotCount() > 0)
        {
            mEvents.onEntityCreated.emit(entity);
        }
        return entity;
    }

    bool destroy(Entity entity)
    {
        if (!isAlive(entity))
        {
            return false;
        }

        // Fire destruction event before removing anything
        if (mEvents.onEntityDestroyed.slotCount() > 0)
        {
            mEvents.onEntityDestroyed.emit(entity);
        }

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
        return mEntities.is_valid(handle);
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

    /// @brief Compute the component mask for an entity on demand.
    /// Iterates registered component stores to check which components
    /// the entity has. O(num_registered_types) — intended for Scheduler
    /// dependency analysis, not per-entity hot paths.
    [[nodiscard]] ComponentMask mask(Entity entity) const noexcept
    {
        if (!isAlive(entity))
        {
            return {};
        }
        ComponentMask result;
        for (auto it = mStores.begin(); it != mStores.end(); ++it)
        {
            if (it.value()->has(entity))
            {
                result.set(it.key());
            }
        }
        return result;
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

        // Fast path: check flat cache first
        if (tid < kStoreCacheSize && mStoreCache[tid] != nullptr)
        {
            return static_cast<ComponentStore<T>*>(mStoreCache[tid]);
        }

        auto* existing = mStores.find(tid);
        if (existing != nullptr)
        {
            auto* raw = static_cast<ComponentStore<T>*>(existing->get());
            if (tid < kStoreCacheSize)
            {
                mStoreCache[tid] = raw;
            }
            return raw;
        }

        auto store = std::make_unique<ComponentStore<T>>();
        auto* raw = store.get();
        mStores.insert(tid, std::move(store));

        if (tid < kStoreCacheSize)
        {
            mStoreCache[tid] = raw;
        }
        return raw;
    }

    template <typename T>
    ComponentStore<T>* getStore()
    {
        const TypeId tid = typeId<T>();

        // Fast path: flat cache
        if (tid < kStoreCacheSize && mStoreCache[tid] != nullptr)
        {
            return static_cast<ComponentStore<T>*>(mStoreCache[tid]);
        }

        auto* val = mStores.find(tid);
        if (val == nullptr)
        {
            return nullptr;
        }
        auto* raw = static_cast<ComponentStore<T>*>(val->get());
        if (tid < kStoreCacheSize)
        {
            mStoreCache[tid] = raw;
        }
        return raw;
    }

    template <typename T>
    const ComponentStore<T>* getStore() const
    {
        const TypeId tid = typeId<T>();

        // Fast path: flat cache (const access)
        if (tid < kStoreCacheSize && mStoreCache[tid] != nullptr)
        {
            return static_cast<const ComponentStore<T>*>(mStoreCache[tid]);
        }

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
    // Data Members
    // =========================================================================

    static constexpr std::size_t kStoreCacheSize = 64;

    /// @brief Entity allocator. Stores a 1-byte dummy payload per entity;
    /// the SlotMap's generational index provides entity identity and ABA safety.
    /// ComponentMask was removed from here to shrink per-entity size from 40→1
    /// bytes (5x less memory, 5x less cache pressure on create).
    fat_p::SlotMap<uint8_t> mEntities;

    fat_p::FastHashMap<TypeId, std::unique_ptr<IComponentStore>> mStores;
    EventBus mEvents;

    /// @brief Flat array cache for O(1) component store lookup by TypeId.
    std::array<IComponentStore*, kStoreCacheSize> mStoreCache{};
};

} // namespace fatp_ecs
