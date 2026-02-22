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

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <typeindex>
#include <typeinfo>

#include <fat_p/BinaryLite.h>
#include <fat_p/FastHashMap.h>
#include <fat_p/SlotMap.h>
#include <fat_p/SmallVector.h>

#include "ComponentMask.h"
#include "ComponentStore.h"
#include "Entity.h"
#include "EventBus.h"
#include "Observer.h"
#include "OwningGroup.h"
#include "RuntimeView.h"
#include "StoragePolicy.h"
#include "TypeId.h"
#include "View.h"

namespace fatp_ecs
{

// Forward declarations — implementations in respective _Impl.h files
class RegistrySnapshot;
class RegistrySnapshotLoader;
class Handle;
class ConstHandle;

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
            it.value()->removeAndNotify(entity, mEvents);
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

        // emplace() delegates to SparseSetWithData::tryEmplace, which does a
        // single sparse lookup and returns nullptr if the entity already has
        // this component. The previous pattern called tryGet() first (one
        // sparse lookup) then emplace() (another), doing the duplicate check
        // twice. Using emplace() directly halves the sparse-array traffic on
        // the hot path.
        //
        // Contract preserved: if entity already has T, the existing component
        // is returned unchanged and no event is fired.
        T* inserted = store->emplaceComponent(entity, std::forward<Args>(args)...);
        if (inserted == nullptr)
        {
            return *store->tryGetComponent(entity);
        }

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

    /**
     * @brief Modify a component in-place and fire onComponentUpdated.
     *
     * Calls func(T&) on the existing component, then emits the
     * onComponentUpdated signal. If the entity does not have T, returns
     * false and func is not called.
     *
     * @tparam T    Component type to patch.
     * @tparam Func Callable with signature void(T&) or compatible.
     * @param entity The entity whose component to modify.
     * @param func   Modifier function applied to the component.
     * @return true if the entity had T and func was called; false otherwise.
     *
     * @note Thread-safety: NOT thread-safe. Use Scheduler or CommandBuffer
     *       for deferred patching from parallel contexts.
     *
     * @example
     * @code
     *   registry.patch<Health>(player, [](Health& h) { h.hp -= 10; });
     * @endcode
     */
    template <typename T, typename Func>
    bool patch(Entity entity, Func&& func)
    {
        auto* store = getStore<T>();
        if (store == nullptr)
        {
            return false;
        }

        T* component = store->tryGetComponent(entity);
        if (component == nullptr)
        {
            return false;
        }

        std::forward<Func>(func)(*component);
        mEvents.emitComponentUpdated<T>(entity, *component);
        return true;
    }

    /**
     * @brief Fire onComponentUpdated for an existing component without modifying it.
     *
     * Useful for marking a component dirty after external modification via get().
     * If the entity does not have T, returns false.
     *
     * @tparam T Component type to signal as updated.
     * @param entity The entity whose component to signal.
     * @return true if the entity had T; false otherwise.
     *
     * @example
     * @code
     *   auto& pos = registry.get<Position>(player);
     *   pos.x += vel.dx;  // Direct modification via get()
     *   registry.patch<Position>(player); // Notify observers
     * @endcode
     */
    template <typename T>
    bool patch(Entity entity)
    {
        auto* store = getStore<T>();
        if (store == nullptr)
        {
            return false;
        }

        T* component = store->tryGetComponent(entity);
        if (component == nullptr)
        {
            return false;
        }

        mEvents.emitComponentUpdated<T>(entity, *component);
        return true;
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
        return store->getComponent(entity);
    }

    template <typename T>
    [[nodiscard]] const T& get(Entity entity) const
    {
        const auto* store = getStore<T>();
        if (store == nullptr)
        {
            throw std::out_of_range("Registry::get: component type not registered");
        }
        return store->getComponent(entity);
    }

    template <typename T>
    [[nodiscard]] T* tryGet(Entity entity)
    {
        auto* store = getStore<T>();
        if (store == nullptr)
        {
            return nullptr;
        }
        return store->tryGetComponent(entity);
    }

    template <typename T>
    [[nodiscard]] const T* tryGet(Entity entity) const
    {
        const auto* store = getStore<T>();
        if (store == nullptr)
        {
            return nullptr;
        }
        return store->tryGetComponent(entity);
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

    /// @brief Create a view that iterates entities with all of Ts.
    template <typename... Ts>
    [[nodiscard]] View<Ts...> view()
    {
        return View<Ts...>(getOrNullStore<Ts>()...);
    }

    /// @brief Create a view that iterates entities with all of Ts and none of Xs.
    /// @example registry.view<Position, Velocity>(Exclude<Frozen>{})
    template <typename... Ts, typename... Xs>
    [[nodiscard]] ViewImpl<std::tuple<Ts...>, std::tuple<Xs...>>
    view(Exclude<Xs...> /*tag*/)
    {
        using V = ViewImpl<std::tuple<Ts...>, std::tuple<Xs...>>;
        return V(typename V::WithExclude{},
                 std::make_tuple(getOrNullStore<Ts>()...),
                 std::make_tuple(getOrNullStore<Xs>()...));
    }

    /**
     * @brief Create a type-erased RuntimeView from TypeId lists.
     *
     * For use when component types are not known at compile time: plugin
     * systems, scripting layers, editors, and generic serialisers.
     *
     * Null stores (TypeIds for types never registered) are handled gracefully:
     * a null include store causes each() to be a no-op; a null exclude store
     * is skipped (type never registered means no entity has it).
     *
     * @param include TypeIds of components entities must have.
     * @param exclude TypeIds of components entities must not have (may be empty).
     * @return RuntimeView ready for iteration.
     *
     * @example
     * @code
     *   auto view = registry.runtimeView(
     *       {typeId<Position>(), typeId<Velocity>()},
     *       {typeId<Frozen>()});
     *   view.each([](Entity e) { ... });
     * @endcode
     */
    [[nodiscard]] RuntimeView
    runtimeView(std::initializer_list<TypeId> include,
                std::initializer_list<TypeId> exclude = {})
    {
        RuntimeView rv;
        for (TypeId tid : include)
        {
            rv.addIncludeStore(getStoreById(tid));
        }
        for (TypeId tid : exclude)
        {
            rv.addExcludeStore(getStoreById(tid));
        }
        return rv;
    }

    /**
     * @brief Span-based overload of runtimeView for pre-built TypeId arrays.
     */
    [[nodiscard]] RuntimeView
    runtimeView(const TypeId* includeBegin, std::size_t includeCount,
                const TypeId* excludeBegin = nullptr,
                std::size_t   excludeCount = 0)
    {
        RuntimeView rv;
        for (std::size_t i = 0; i < includeCount; ++i)
        {
            rv.addIncludeStore(getStoreById(includeBegin[i]));
        }
        for (std::size_t i = 0; i < excludeCount; ++i)
        {
            rv.addExcludeStore(getStoreById(excludeBegin[i]));
        }
        return rv;
    }

    // =========================================================================
    // Observers
    // =========================================================================

    /**
     * @brief Create an Observer wired to the specified component event triggers.
     *
     * @tparam Triggers Mix of OnAdded<T>, OnRemoved<T>, and OnUpdated<T> tags.
     *
     * The observer accumulates dirty entities until clear() is called. It
     * automatically connects to onEntityDestroyed to remove stale handles.
     *
     * @return Observer with all requested connections live.
     *
     * @example
     * @code
     *   // Dirty when Position is added or patched:
     *   auto obs = registry.observe(OnAdded<Position>{}, OnUpdated<Position>{});
     *
     *   // Dirty when Velocity is added, Position or Velocity is patched:
     *   auto obs2 = registry.observe(
     *       OnAdded<Velocity>{},
     *       OnUpdated<Position>{},
     *       OnUpdated<Velocity>{});
     *
     *   // Each frame:
     *   obs.each([&](Entity e) { ... });
     *   obs.clear();
     * @endcode
     */
    template <typename... Triggers>
    [[nodiscard]] Observer observe(Triggers... triggers)
    {
        Observer obs;
        obs.connectEntityDestroyed(mEvents);
        (connectTrigger(obs, triggers), ...);
        return obs;
    }

    // =========================================================================
    // Owning Groups
    // =========================================================================

    /**
     * @brief Create (or retrieve) an owning group over the given component types.
     *
     * An owning group maintains a contiguous prefix in each owned store's
     * dense array, so that group::each() can walk all component arrays
     * simultaneously with zero sparse lookups — pure sequential array access.
     *
     * @tparam Ts Owned component types (must be 2 or more).
     *
     * @return Reference to the OwningGroup. The Registry owns the group's
     *         lifetime; callers may hold the reference as long as the
     *         Registry is alive.
     *
     * @note Each component type may be owned by at most one group per Registry.
     *       Calling group<A, B>() and group<A, C>() on the same Registry will
     *       assert in debug builds and is undefined behaviour in release builds.
     *
     * @example
     * @code
     *   auto& grp = registry.group<Position, Velocity>();
     *
     *   grp.each([](Entity e, Position& p, Velocity& v) {
     *       p.x += v.dx;
     *       p.y += v.dy;
     *   });
     * @endcode
     */
    template <typename... Ts>
    [[nodiscard]] OwningGroup<Ts...>& group()
    {
        const TypeId key = groupKey<Ts...>();

        auto* existing = mGroups.find(key);
        if (existing != nullptr)
        {
            return *static_cast<OwningGroup<Ts...>*>(existing->get());
        }

        // Assert no ownership conflicts (each TypeId used by at most one group).
        assertNoOwnershipConflicts<Ts...>();

        // Mark each type as owned.
        (markOwned(typeId<Ts>()), ...);

        // Create the group.
        auto grp = std::make_unique<OwningGroup<Ts...>>(
            ensureStore<Ts>()..., mEvents);
        auto* raw = static_cast<OwningGroup<Ts...>*>(grp.get());
        mGroups.insert(key, std::move(grp));
        return *raw;
    }

    // =========================================================================
    // Sorting
    // =========================================================================

    /**
     * @brief Sort component store T's dense array by a comparator.
     *
     * After this call, iterating view<T>() will visit entities in the order
     * defined by @p comparator. The comparator receives two component values
     * and returns true if the first should come before the second.
     *
     * Uses an O(n log n) indirect sort followed by O(n) in-place permutation
     * application, so each entity is moved at most once.
     *
     * @tparam T         Component type whose store is sorted.
     * @tparam Comparator Callable with signature bool(const T&, const T&).
     * @param  comparator Strict weak ordering comparator.
     *
     * @note Do NOT sort a store owned by an OwningGroup — doing so corrupts
     *       the group's packed-prefix invariant. Sort before creating groups,
     *       or sort only component types that are not group-owned.
     *
     * @note If T has never been registered (no entity ever had T), this is
     *       a no-op.
     *
     * @example
     * @code
     *   // Sort entities by decreasing health (injured-first):
     *   registry.sort<Health>([](const Health& a, const Health& b) {
     *       return a.hp < b.hp;
     *   });
     *
     *   // Sort by entity depth for back-to-front rendering:
     *   registry.sort<RenderDepth>([](const RenderDepth& a, const RenderDepth& b) {
     *       return a.z > b.z;
     *   });
     * @endcode
     */
    template <typename T, typename Comparator>
    void sort(Comparator&& comparator)
    {
        auto* store = getStore<T>();
        if (store == nullptr || store->size() <= 1)
        {
            return;
        }
        sortStore(*store, std::forward<Comparator>(comparator));
    }

    /**
     * @brief Sort store B so its entities appear in the same relative order
     *        as they do in store A.
     *
     * Entities present in B but absent from A are left at the tail of B in
     * their original relative order. Entities present in A but absent from B
     * are ignored.
     *
     * This is the primary tool for aligning two stores so that View<A, B>
     * iteration is cache-friendly: after sort<A, B>(), the entities shared
     * by both stores appear at matching dense indices in B (relative to A's
     * order).
     *
     * @tparam A Pivot store — its order is the target.
     * @tparam B Store to reorder to match A.
     *
     * @note Do NOT use on group-owned stores (see sort<T> caveat above).
     * @note If either type is unregistered, this is a no-op.
     *
     * @example
     * @code
     *   // Sort by position, then align velocity to match:
     *   registry.sort<Position>([](const Position& a, const Position& b) {
     *       return a.x < b.x;
     *   });
     *   registry.sort<Position, Velocity>(); // Velocity now mirrors Position order
     *
     *   // View<Position, Velocity> now walks both arrays sequentially.
     * @endcode
     */
    template <typename A, typename B>
    void sort()
    {
        auto* pivotStore = getStore<A>();
        auto* followStore = getStore<B>();
        if (pivotStore == nullptr || followStore == nullptr)
        {
            return;
        }
        if (followStore->size() <= 1)
        {
            return;
        }
        sortStoreToMatch(*pivotStore, *followStore);
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

    // =========================================================================
    // Entity copy
    // =========================================================================

    /**
     * @brief Copy all components from src to dst within this registry.
     *
     * For each component store that contains src, the component value is
     * copied to dst. If dst already has the component it is overwritten in-place
     * and onComponentUpdated fires. If dst does not have it, it is added and
     * onComponentAdded fires.
     *
     * src and dst must both be alive. Copying an entity to itself is a no-op
     * (the copy-into-self path is detected and skipped per-store).
     *
     * @return Number of components copied. 0 if src has no components or is not alive.
     *
     * @note For cross-registry copy, use Snapshot/SnapshotLoader or manually
     *       iterate allEntities() + component accessors.
     * @note Thread-safety: NOT thread-safe.
     */
    std::size_t copy(Entity src, Entity dst)
    {
        if (src == dst || !isAlive(src) || !isAlive(dst))
        {
            return 0;
        }

        std::size_t count = 0;
        for (auto it = mStores.begin(); it != mStores.end(); ++it)
        {
            if (it.value()->copyTo(src, dst, mEvents))
            {
                ++count;
            }
        }
        return count;
    }

        // =========================================================================
    // Snapshot / Serialization
    // =========================================================================

    /**
     * @brief Begin serializing registry state into enc.
     *
     * Writes the snapshot header (magic, version, entity table) immediately,
     * then returns a RegistrySnapshot the caller uses to serialize individual
     * component types.
     *
     * @param enc  Encoder to write into. Must outlive the returned snapshot.
     * @return     RegistrySnapshot for per-component serialization calls.
     *
     * @note Defined in Snapshot_Impl.h (included from FatpEcs.h).
     * @note Thread-safety: NOT thread-safe.
     */
    [[nodiscard]] RegistrySnapshot snapshot(fat_p::binary::Encoder& enc);

    /**
     * @brief Begin restoring registry state from dec.
     *
     * Clears this registry, reads the snapshot header, and recreates all
     * entities (building the old→new EntityMap). Returns a RegistrySnapshotLoader
     * the caller uses to restore individual component types.
     *
     * @param dec  Decoder positioned at the start of a snapshot buffer.
     * @return     RegistrySnapshotLoader for per-component deserialization calls.
     *
     * @note Defined in Snapshot_Impl.h (included from FatpEcs.h).
     * @note Thread-safety: NOT thread-safe.
     */
    [[nodiscard]] RegistrySnapshotLoader snapshotLoader(fat_p::binary::Decoder& dec);

    /**
     * @brief Read-only access to a typed component store, or nullptr if absent.
     *
     * Used by RegistrySnapshot to iterate component data without type erasure.
     * Returns nullptr if no entity has ever had component T.
     */
    template <typename T>
    [[nodiscard]] const TypedIComponentStore<T>* tryGetStore() const
    {
        return getStore<T>();
    }

    // =========================================================================
    // Storage policy registration
    // =========================================================================

    /**
     * @brief Pre-create the component store for T using a custom storage policy.
     *
     * Must be called BEFORE any entity has component T. If the store already
     * exists (because add<T>() was called first), this call asserts in debug
     * and is a no-op in release — the default-policy store continues to be used.
     *
     * @tparam T      Component type.
     * @tparam Policy Storage policy template (e.g. AlignedStoragePolicy<64>::type).
     *
     * @code
     * // 64-byte aligned storage for SIMD processing:
     * registry.useStorage<Transform, AlignedStoragePolicy<64>::type>();
     *
     * // Thread-safe shared-read storage:
     * registry.useStorage<Health, ConcurrentStoragePolicy<fat_p::SharedMutexPolicy>::type>();
     * @endcode
     */
    template <typename T, template <typename> class Policy>
        requires StoragePolicy<Policy>
    void useStorage()
    {
        const TypeId tid = typeId<T>();

        assert(mStores.find(tid) == nullptr &&
               "useStorage<T, Policy>() called after component T was already added. "
               "Call useStorage() before the first add<T>().");

        auto store = std::make_unique<ComponentStore<T, Policy>>();
        auto* raw = store.get();
        mStores.insert(tid, std::move(store));

        if (tid < kStoreCacheSize)
        {
            mStoreCache[tid] = raw;
        }
    }

    /**
     * @brief Pre-create an aligned storage store for T.
     *
     * Convenience wrapper for the common case of SIMD/cache-line alignment.
     *
     * @tparam T         Component type.
     * @tparam Alignment Byte alignment (must be power of two, >= alignof(T)).
     *
     * @code
     * registry.useAlignedStorage<Transform, 64>();   // cache-line aligned
     * registry.useAlignedStorage<Velocity, 32>();    // AVX2
     * @endcode
     */
    template <typename T, std::size_t Alignment>
    void useAlignedStorage()
    {
        useStorage<T, AlignedStoragePolicy<Alignment>::template Policy>();
    }

    // =========================================================================
    // Handle factories
    // =========================================================================

    /**
     * @brief Create a mutable handle to an existing entity.
     *
     * The handle is a lightweight (Registry*, Entity) pair. It does not own
     * the entity — the entity must be destroyed explicitly via handle.destroy()
     * or registry.destroy(entity).
     *
     * @note Defined in EntityHandle_Impl.h (included from FatpEcs.h).
     */
    [[nodiscard]] Handle handle(Entity entity) noexcept;

    /**
     * @brief Create a read-only handle to an existing entity.
     *
     * @note Defined in EntityHandle_Impl.h (included from FatpEcs.h).
     */
    [[nodiscard]] ConstHandle constHandle(Entity entity) const noexcept;

private:
    // =========================================================================
    // Internal: Store Management
    // =========================================================================

    template <typename T>
    TypedIComponentStore<T>* ensureStore()
    {
        const TypeId tid = typeId<T>();

        // Fast path: check flat cache first
        if (tid < kStoreCacheSize && mStoreCache[tid] != nullptr)
        {
            return static_cast<TypedIComponentStore<T>*>(mStoreCache[tid]);
        }

        auto* existing = mStores.find(tid);
        if (existing != nullptr)
        {
            auto* raw = static_cast<TypedIComponentStore<T>*>(existing->get());
            if (tid < kStoreCacheSize)
            {
                mStoreCache[tid] = raw;
            }
            return raw;
        }

        auto store = std::make_unique<ComponentStore<T>>();
        auto* raw = static_cast<TypedIComponentStore<T>*>(store.get());
        mStores.insert(tid, std::move(store));

        if (tid < kStoreCacheSize)
        {
            mStoreCache[tid] = raw;
        }
        return raw;
    }

    template <typename T>
    TypedIComponentStore<T>* getStore()
    {
        const TypeId tid = typeId<T>();

        // Fast path: flat cache
        if (tid < kStoreCacheSize && mStoreCache[tid] != nullptr)
        {
            return static_cast<TypedIComponentStore<T>*>(mStoreCache[tid]);
        }

        auto* val = mStores.find(tid);
        if (val == nullptr)
        {
            return nullptr;
        }
        auto* raw = static_cast<TypedIComponentStore<T>*>(val->get());
        if (tid < kStoreCacheSize)
        {
            mStoreCache[tid] = raw;
        }
        return raw;
    }

    template <typename T>
    const TypedIComponentStore<T>* getStore() const
    {
        const TypeId tid = typeId<T>();

        if (tid < kStoreCacheSize && mStoreCache[tid] != nullptr)
        {
            return static_cast<const TypedIComponentStore<T>*>(mStoreCache[tid]);
        }

        const auto* val = mStores.find(tid);
        if (val == nullptr)
        {
            return nullptr;
        }
        return static_cast<const TypedIComponentStore<T>*>(val->get());
    }

    template <typename T>
    TypedIComponentStore<T>* getOrNullStore()
    {
        return getStore<T>();
    }

    // =========================================================================
    // Sort helpers
    // =========================================================================

    /**
     * @brief Indirect-sort a ComponentStore<T> by a comparator.
     *
     * 1. Build permutation perm[0..n-1] = {0, 1, ..., n-1}.
     * 2. std::sort perm by comparator(data[perm[i]], data[perm[j]]).
     * 3. Apply the permutation in-place using cycle decomposition:
     *    each dense index is visited at most twice — once to start the cycle,
     *    once to close it — so the total number of swapDenseEntries calls is O(n).
     *
     * The cycle-decomposition trick avoids the O(n²) swap count that a naive
     * selection sort would incur.
     */
    template <typename T, typename Comparator>
    static void sortStore(TypedIComponentStore<T>& store, Comparator&& comparator)
    {
        const std::size_t n = store.size();

        // Build permutation sorted by comparator applied to component data.
        std::vector<std::size_t> perm(n);
        for (std::size_t i = 0; i < n; ++i) perm[i] = i;

        std::sort(perm.begin(), perm.end(),
                  [&](std::size_t a, std::size_t b) {
                      return comparator(store.dataAtUnchecked(a),
                                        store.dataAtUnchecked(b));
                  });

        // Apply permutation via cycle decomposition.
        // visited[i] = true once index i has been placed in its final position.
        std::vector<bool> visited(n, false);

        for (std::size_t i = 0; i < n; ++i)
        {
            if (visited[i] || perm[i] == i)
            {
                visited[i] = true;
                continue;
            }

            // Follow the cycle starting at i.
            std::size_t current = i;
            while (!visited[current])
            {
                const std::size_t next = perm[current];
                visited[current] = true;
                if (next != i && !visited[next])
                {
                    store.swapDenseEntries(current, next);
                    current = next;
                }
                else
                {
                    break;
                }
            }
        }
    }

    /**
     * @brief Rearrange followStore so its entities appear in the same relative
     *        order as they do in pivotStore.
     *
     * Algorithm: iterate pivot's dense array left-to-right. For each entity
     * that is also in follow, swap it into the next consecutive position
     * (starting from index 0). Entities in follow but absent from pivot remain
     * at the tail in their original order.
     *
     * Total swaps: at most min(|A|, |B|) — linear.
     */
    template <typename A, typename B>
    static void sortStoreToMatch(TypedIComponentStore<A>& pivotStore,
                                 TypedIComponentStore<B>& followStore)
    {
        std::size_t nextPos = 0; // next slot to fill in followStore

        const std::size_t pivotCount = pivotStore.size();
        for (std::size_t pi = 0; pi < pivotCount; ++pi)
        {
            const Entity entity = pivotStore.dense()[pi];
            const std::size_t fi = followStore.getDenseIndex(entity);

            if (fi == followStore.size())
            {
                continue; // entity not in followStore — skip
            }

            if (fi != nextPos)
            {
                followStore.swapDenseEntries(fi, nextPos);
            }
            ++nextPos;
        }
    }

    // =========================================================================
    // Observer trigger dispatch
    // =========================================================================

    template <typename T>
    void connectTrigger(Observer& obs, OnAdded<T>)
    {
        obs.connectAdded<T>(mEvents);
    }

    template <typename T>
    void connectTrigger(Observer& obs, OnRemoved<T>)
    {
        obs.connectRemoved<T>(mEvents);
    }

    template <typename T>
    void connectTrigger(Observer& obs, OnUpdated<T>)
    {
        obs.connectUpdated<T>(mEvents);
    }

    // =========================================================================
    // Group helpers
    // =========================================================================

    /// @brief Stable composite key for a group's type set.
    /// Combines all TypeIds into a single hash. Uses XOR + rotation to be
    /// order-independent within reasonable assumptions (group<A,B> == group<B,A>
    /// is not required — same order required at call site, matching EnTT).
    template <typename... Ts>
    TypeId groupKey()
    {
        // FNV-1a-inspired fold over TypeIds, order-dependent.
        TypeId key = 0xcbf29ce484222325ULL;
        ((key = (key ^ typeId<Ts>()) * 0x100000001b3ULL), ...);
        return key;
    }

    void markOwned(TypeId tid)
    {
        mOwnedTypes.insert(tid, true);
    }

    template <typename... Ts>
    void assertNoOwnershipConflicts()
    {
        (assertNotOwned(typeId<Ts>()), ...);
    }

    void assertNotOwned([[maybe_unused]] TypeId tid)
    {
        assert(mOwnedTypes.find(tid) == nullptr &&
               "OwningGroup: component type already owned by another group");
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

    /// @brief Type-erased owning groups, keyed by group signature hash.
    fat_p::FastHashMap<TypeId, std::unique_ptr<IOwningGroup>> mGroups;

    /// @brief TypeIds claimed by an owning group — for conflict detection.
    fat_p::FastHashMap<TypeId, bool> mOwnedTypes;

    // =========================================================================
    // Store lookup by TypeId (used by runtimeView())
    // =========================================================================

    [[nodiscard]] IComponentStore* getStoreById(TypeId tid) noexcept
    {
        if (tid < kStoreCacheSize && mStoreCache[tid] != nullptr)
        {
            return mStoreCache[tid];
        }
        auto* val = mStores.find(tid);
        if (val == nullptr)
        {
            return nullptr;
        }
        auto* raw = val->get();
        if (tid < kStoreCacheSize)
        {
            mStoreCache[tid] = raw;
        }
        return raw;
    }
};

} // namespace fatp_ecs
