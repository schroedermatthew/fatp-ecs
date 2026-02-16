#pragma once

/**
 * @file ComponentStore.h
 * @brief Type-erased component storage backed by SparseSetWithData.
 */

// FAT-P components used:
// - SparseSetWithData: Per-component-type storage with O(1) add/remove/get
//   - indexOf(): Keeps the parallel entity vector in sync on erase
//
// Each ComponentStore<T> maintains a parallel std::vector<Entity> alongside
// the SparseSet's dense array. The SparseSet stores uint32_t indices (losing
// the Entity generation); the parallel vector stores full 64-bit entities.
// Views read from this vector to yield correct generational Entity handles.
//
// The parallel vector mirrors the SparseSet's swap-with-back erasure via
// indexOf(), which returns the dense index of a given sparse key before
// the erase modifies it.

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

/// @brief Abstract interface for type-erased component storage.
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

/**
 * @brief Typed component storage wrapping SparseSetWithData.
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
        bool inserted = mStorage.emplace(EntityTraits::toIndex(entity),
                                         std::forward<Args>(args)...);
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

    /// @brief Returns the parallel entity vector (full Entity with generation).
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
    std::vector<Entity> mEntities;  // Parallel to dense: full Entity with generation
};

} // namespace fatp_ecs
