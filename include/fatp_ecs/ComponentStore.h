#pragma once

/**
 * @file ComponentStore.h
 * @brief Type-erased component storage backed by SparseSetWithData.
 */

// FAT-P components used:
// - SparseSetWithData: Per-component-type storage with O(1) add/remove/get
//   - EntityIndex policy: Keys on full 64-bit Entity, indexes sparse array
//     by the 32-bit slot index. The dense array stores full Entity values,
//     eliminating the need for a parallel entity vector.
//   - tryEmplace(): Returns pointer to inserted data, avoiding the
//     double-lookup of emplace() + tryGet().

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <fat_p/SparseSet.h>

#include "Entity.h"
#include "EventBus.h"

namespace fatp_ecs
{
// (EventBus is fully defined via the include above)

// =============================================================================
// IComponentStore - Type-Erased Interface
// =============================================================================

/// @brief Abstract interface for type-erased component storage.
class IComponentStore
{
public:
    virtual ~IComponentStore() = default;

    [[nodiscard]] virtual bool has(Entity entity) const noexcept = 0;
    virtual bool remove(Entity entity) = 0;

    /// @brief Remove entity and fire onComponentRemoved signal via the EventBus.
    ///        Used by Registry::destroy() so groups and observers are notified.
    virtual bool removeAndNotify(Entity entity, EventBus& events) = 0;

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool empty() const noexcept = 0;
    virtual void clear() = 0;

    /// @brief Pointer to the dense entity array. Used by RuntimeView to iterate
    ///        the pivot store without knowing the component type.
    [[nodiscard]] virtual const Entity* denseEntities() const noexcept = 0;

    /// @brief Number of entries in the dense entity array (== size()).
    [[nodiscard]] virtual std::size_t denseEntityCount() const noexcept = 0;

    /// @brief Copy the component from src to dst if src has it.
    ///        If dst already has the component it is replaced.
    ///        Fires onComponentAdded via events if the component is newly added.
    ///        Returns false if src does not have the component.
    virtual bool copyTo(Entity src, Entity dst, EventBus& events) = 0;

    IComponentStore() = default;
    IComponentStore(const IComponentStore&) = delete;
    IComponentStore& operator=(const IComponentStore&) = delete;
    IComponentStore(IComponentStore&&) = delete;
    IComponentStore& operator=(IComponentStore&&) = delete;
};

// =============================================================================
// ComponentStore<T> - Concrete Typed Storage
// =============================================================================

/**
 * @brief Typed component storage wrapping SparseSetWithData.
 *
 * Uses EntityIndex policy so the SparseSet keys on full 64-bit Entity
 * values while indexing the sparse array by the 32-bit slot index. The
 * dense array stores full Entity values directly, providing generational
 * handles for Views without a parallel vector.
 *
 * @tparam T The component data type.
 *
 * @note Thread-safety: NOT thread-safe. The Registry serializes access.
 */
template <typename T>
class ComponentStore : public IComponentStore
{
public:
    using DataType = T;
    using StorageType = fat_p::SparseSetWithData<Entity, T, EntityIndex>;

    ComponentStore() = default;

    // =========================================================================
    // IComponentStore interface
    // =========================================================================

    [[nodiscard]] bool has(Entity entity) const noexcept override
    {
        return mStorage.contains(entity);
    }

    bool remove(Entity entity) override
    {
        return mStorage.erase(entity);
    }

    bool removeAndNotify(Entity entity, EventBus& events) override
    {
        if (!mStorage.contains(entity))
        {
            return false;
        }
        events.emitComponentRemoved<T>(entity);
        return mStorage.erase(entity);
    }

    [[nodiscard]] std::size_t size() const noexcept override
    {
        return mStorage.size();
    }

    [[nodiscard]] bool empty() const noexcept override
    {
        return mStorage.empty();
    }

    void clear() override
    {
        mStorage.clear();
    }

    [[nodiscard]] const Entity* denseEntities() const noexcept override
    {
        return mStorage.dense().data();
    }

    [[nodiscard]] std::size_t denseEntityCount() const noexcept override
    {
        return mStorage.dense().size();
    }

    bool copyTo(Entity src, Entity dst, EventBus& events) override
    {
        const T* srcComp = mStorage.tryGet(src);
        if (srcComp == nullptr)
        {
            return false;
        }

        // If dst already has the component, overwrite in-place (no re-insert).
        T* dstComp = mStorage.tryGet(dst);
        if (dstComp != nullptr)
        {
            *dstComp = *srcComp;
            events.emitComponentUpdated<T>(dst, *dstComp);
        }
        else
        {
            T* inserted = mStorage.tryEmplace(dst, *srcComp);
            if (inserted != nullptr)
            {
                events.emitComponentAdded<T>(dst, *inserted);
            }
        }
        return true;
    }

    // =========================================================================
    // Typed operations
    // =========================================================================

    /**
     * @brief Inserts a component for an entity via copy.
     *
     * @param entity    The entity to associate the component with.
     * @param component Component value to copy.
     * @return Pointer to inserted data, or nullptr if entity already had one.
     */
    T* add(Entity entity, const T& component)
    {
        return mStorage.tryEmplace(entity, component);
    }

    /**
     * @brief Inserts a component for an entity via move.
     *
     * @param entity    The entity to associate the component with.
     * @param component Component value to move.
     * @return Pointer to inserted data, or nullptr if entity already had one.
     */
    T* add(Entity entity, T&& component)
    {
        return mStorage.tryEmplace(entity, std::move(component));
    }

    /**
     * @brief Constructs a component in-place for an entity.
     *
     * @tparam Args Constructor argument types for T.
     * @param entity The entity to associate the component with.
     * @param args   Arguments forwarded to T's constructor.
     * @return Pointer to inserted data, or nullptr if entity already had one.
     */
    template <typename... Args>
    T* emplace(Entity entity, Args&&... args)
    {
        return mStorage.tryEmplace(entity, std::forward<Args>(args)...);
    }

    [[nodiscard]] T* tryGet(Entity entity) noexcept
    {
        return mStorage.tryGet(entity);
    }

    [[nodiscard]] const T* tryGet(Entity entity) const noexcept
    {
        return mStorage.tryGet(entity);
    }

    [[nodiscard]] T& get(Entity entity)
    {
        return mStorage.get(entity);
    }

    [[nodiscard]] const T& get(Entity entity) const
    {
        return mStorage.get(entity);
    }

    // =========================================================================
    // Direct storage access (for View iteration)
    // =========================================================================

    /// @brief Returns the dense key array (full Entity values with generation).
    [[nodiscard]] const std::vector<Entity>& dense() const noexcept
    {
        return mStorage.dense();
    }

    [[nodiscard]] const std::vector<T>& data() const noexcept
    {
        return mStorage.data();
    }

    [[nodiscard]] T& dataAt(std::size_t denseIndex)
    {
        return mStorage.dataAt(denseIndex);
    }

    [[nodiscard]] const T& dataAt(std::size_t denseIndex) const
    {
        return mStorage.dataAt(denseIndex);
    }

    // =========================================================================
    // Unchecked access (for View iteration hot paths)
    // =========================================================================

    /// @brief Direct data access by dense index — no bounds check.
    /// @warning Caller must ensure denseIndex < size().
    [[nodiscard]] T& dataAtUnchecked(std::size_t denseIndex) noexcept
    {
        return mStorage.data()[denseIndex];
    }

    [[nodiscard]] const T& dataAtUnchecked(std::size_t denseIndex) const noexcept
    {
        return mStorage.data()[denseIndex];
    }

    /// @brief Get component by entity without re-verifying membership.
    /// Combines the sparse→dense lookup into a single step. The caller
    /// must have already confirmed the entity is in this store (via has()).
    /// @warning Undefined behavior if entity is not in the store.
    [[nodiscard]] T& getUnchecked(Entity entity) noexcept
    {
        const auto sparseIdx = EntityIndex::index(entity);
        const auto denseIdx = mStorage.sparse()[sparseIdx];
        return mStorage.data()[denseIdx];
    }

    [[nodiscard]] const T& getUnchecked(Entity entity) const noexcept
    {
        const auto sparseIdx = EntityIndex::index(entity);
        const auto denseIdx = mStorage.sparse()[sparseIdx];
        return mStorage.data()[denseIdx];
    }

    [[nodiscard]] StorageType& storage() noexcept
    {
        return mStorage;
    }

    [[nodiscard]] const StorageType& storage() const noexcept
    {
        return mStorage;
    }

    /// @brief Returns the dense entity array. Used by OwningGroup to reorder entries.
    [[nodiscard]] std::vector<Entity>& mutableDense() noexcept
    {
        return const_cast<std::vector<Entity>&>(mStorage.dense());
    }

    // =========================================================================
    // Group support: in-place dense array reordering
    //
    // OwningGroup maintains a contiguous prefix [0, groupSize) in each owned
    // store where all group entities are packed. These two primitives are the
    // only operations needed to maintain that invariant.
    // =========================================================================

    /**
     * @brief Returns the dense index of entity, or size() if not present.
     */
    [[nodiscard]] std::size_t getDenseIndex(Entity entity) const noexcept
    {
        const auto sparseIdx = EntityIndex::index(entity);
        const auto& sp = mStorage.sparse();
        if (sparseIdx >= sp.size())
        {
            return mStorage.size(); // sentinel: not present
        }
        const std::size_t di = sp[sparseIdx];
        // Validate: the dense slot must point back to this entity.
        if (di >= mStorage.dense().size() ||
            EntityIndex::index(mStorage.dense()[di]) != sparseIdx)
        {
            return mStorage.size();
        }
        return di;
    }

    /**
     * @brief Swap the entities at dense positions i and j in-place.
     *
     * Updates both dense[] and data[] arrays, and patches the two sparse
     * entries so lookups remain correct.
     *
     * @pre i < size() && j < size()
     */
    void swapDenseEntries(std::size_t i, std::size_t j) noexcept
    {
        if (i == j) return;

        auto& dens = mutableDense();
        auto& dat  = mStorage.data();
        auto& sp   = mStorage.sparse();

        std::swap(dens[i], dens[j]);
        std::swap(dat[i], dat[j]);
        sp[EntityIndex::index(dens[i])] = static_cast<uint32_t>(i);
        sp[EntityIndex::index(dens[j])] = static_cast<uint32_t>(j);
    }

    // =========================================================================
    // Raw-pointer accessors for View iteration pre-caching
    //
    // View::eachWithPivot loads these into local variables before the hot loop.
    // Working with raw stack-local pointers eliminates Clang's conservative
    // per-iteration reloads of the SparseSetWithData's internal vector metadata
    // (data pointer, size), which it cannot hoist when they are accessed through
    // the ComponentStore pointer due to potential aliasing with loop body writes.
    // =========================================================================

    /// @brief Raw pointer to the dense entity array. Valid until next add/remove.
    [[nodiscard]] const Entity* densePtr() const noexcept
    {
        return mStorage.dense().data();
    }

    /// @brief Number of entries in the dense array (== size()).
    [[nodiscard]] std::size_t denseCount() const noexcept
    {
        return mStorage.dense().size();
    }

    /// @brief Raw pointer to the sparse index array. Valid until next add/remove.
    [[nodiscard]] const uint32_t* sparsePtr() const noexcept
    {
        return mStorage.sparse().data();
    }

    /// @brief Number of entries in the sparse array (capacity, not entity count).
    [[nodiscard]] std::size_t sparseCount() const noexcept
    {
        return mStorage.sparse().size();
    }

    /// @brief Raw pointer to the component data array. Valid until next add/remove.
    [[nodiscard]] T* componentDataPtr() noexcept
    {
        return mStorage.data().data();
    }

    [[nodiscard]] const T* componentDataPtr() const noexcept
    {
        return mStorage.data().data();
    }

private:
    StorageType mStorage;
};

} // namespace fatp_ecs
