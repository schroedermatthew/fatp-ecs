#pragma once

/**
 * @file ComponentStore.h
 * @brief Type-erased component storage backed by SparseSetWithData.
 *
 * @details
 * Each concrete ComponentStore<T> wraps a SparseSetWithData<uint32_t, T>.
 *
 * Phase 2 addition: Each ComponentStore maintains a parallel vector of
 * full Entity values (including generation) alongside the SparseSet's
 * dense array. This allows Views to reconstruct complete Entity handles
 * during iteration, which is critical for generational safety.
 *
 * The parallel entity vector is kept in sync via SparseSetWithData::indexOf(),
 * which enables mirroring the swap-with-back erasure pattern.
 *
 * FAT-P components used:
 * - SparseSetWithData: Per-component-type storage
 *   - indexOf(): Keeps the parallel entity vector in sync on erase
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include <fat_p/SparseSet.h>

#include "Entity.h"

namespace fatp_ecs
{

// =============================================================================
// IComponentStore - Type-Erased Interface
// =============================================================================

class IComponentStore
{
public:
    virtual ~IComponentStore() = default;

    [[nodiscard]] virtual bool has(Entity entity) const noexcept = 0;
    virtual bool remove(Entity entity) = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool empty() const noexcept = 0;
    virtual void clear() = 0;

    IComponentStore() = default;
    IComponentStore(const IComponentStore&) = delete;
    IComponentStore& operator=(const IComponentStore&) = delete;
    IComponentStore(IComponentStore&&) = delete;
    IComponentStore& operator=(IComponentStore&&) = delete;
};

// =============================================================================
// ComponentStore<T> - Concrete Typed Storage
// =============================================================================

template <typename T>
class ComponentStore : public IComponentStore
{
public:
    using DataType = T;
    using IndexType = EntityTraits::IndexType;
    using StorageType = fat_p::SparseSetWithData<IndexType, T>;

    ComponentStore() = default;

    // =========================================================================
    // IComponentStore interface
    // =========================================================================

    [[nodiscard]] bool has(Entity entity) const noexcept override
    {
        return mStorage.contains(EntityTraits::toIndex(entity));
    }

    bool remove(Entity entity) override
    {
        IndexType idx = EntityTraits::toIndex(entity);
        if (!mStorage.contains(idx))
        {
            return false;
        }

        // Get the dense index BEFORE erasing so we can mirror swap-with-back
        std::size_t denseIdx = mStorage.indexOf(idx);
        std::size_t lastIdx = mStorage.size() - 1;

        // Erase from SparseSet (does swap-with-back internally)
        mStorage.erase(idx);

        // Mirror swap-with-back in entity vector
        if (denseIdx < lastIdx)
        {
            mEntities[denseIdx] = mEntities[lastIdx];
        }
        mEntities.pop_back();

        return true;
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
        mEntities.clear();
    }

    // =========================================================================
    // Typed operations
    // =========================================================================

    bool add(Entity entity, const T& component)
    {
        bool inserted = mStorage.insert(EntityTraits::toIndex(entity), component);
        if (inserted)
        {
            mEntities.push_back(entity);
        }
        return inserted;
    }

    bool add(Entity entity, T&& component)
    {
        bool inserted = mStorage.insert(EntityTraits::toIndex(entity), std::move(component));
        if (inserted)
        {
            mEntities.push_back(entity);
        }
        return inserted;
    }

    template <typename... Args>
    bool emplace(Entity entity, Args&&... args)
    {
        bool inserted = mStorage.emplace(EntityTraits::toIndex(entity), std::forward<Args>(args)...);
        if (inserted)
        {
            mEntities.push_back(entity);
        }
        return inserted;
    }

    [[nodiscard]] T* tryGet(Entity entity) noexcept
    {
        return mStorage.tryGet(EntityTraits::toIndex(entity));
    }

    [[nodiscard]] const T* tryGet(Entity entity) const noexcept
    {
        return mStorage.tryGet(EntityTraits::toIndex(entity));
    }

    [[nodiscard]] T& get(Entity entity)
    {
        return mStorage.get(EntityTraits::toIndex(entity));
    }

    [[nodiscard]] const T& get(Entity entity) const
    {
        return mStorage.get(EntityTraits::toIndex(entity));
    }

    // =========================================================================
    // Direct storage access (for View iteration)
    // =========================================================================

    [[nodiscard]] const std::vector<IndexType>& dense() const noexcept
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

    /**
     * @brief Returns the parallel entity vector (full Entity with generation).
     *
     * entities()[i] is the full Entity handle for the component at data()[i].
     * Views use this to yield correct Entity handles during iteration.
     */
    [[nodiscard]] const std::vector<Entity>& entities() const noexcept
    {
        return mEntities;
    }

    [[nodiscard]] StorageType& storage() noexcept
    {
        return mStorage;
    }

    [[nodiscard]] const StorageType& storage() const noexcept
    {
        return mStorage;
    }

private:
    StorageType mStorage;
    std::vector<Entity> mEntities;  ///< Parallel to dense: full Entity with generation
};

} // namespace fatp_ecs
