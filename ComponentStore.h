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
};

} // namespace fatp_ecs
