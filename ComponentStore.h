#pragma once

/**
 * @file ComponentStore.h
 * @brief Type-erased component storage backed by SparseSetWithData.
 *
 * @details
 * IComponentStore provides a type-erased interface for the Registry's
 * component storage map. Each concrete ComponentStore<T> wraps a
 * fat_p::SparseSetWithData<uint32_t, T>, giving O(1) add/remove/get
 * and cache-friendly dense iteration.
 *
 * The type erasure is necessary because the Registry must store
 * heterogeneous component types in a single FastHashMap keyed by
 * type ID. The concrete ComponentStore<T> is accessed via
 * static_cast when the type is known at the call site.
 *
 * FAT-P components used:
 * - SparseSetWithData: Per-component-type storage (the core data structure)
 *
 * @note Thread Safety: NOT thread-safe. The Registry serializes access.
 */

#include <cstddef>
#include <cstdint>

#include <fat_p/SparseSet.h>

#include "Entity.h"

namespace fatp_ecs
{

// =============================================================================
// IComponentStore - Type-Erased Interface
// =============================================================================

/**
 * @brief Abstract interface for type-erased component storage.
 *
 * @details
 * The Registry holds component stores as unique_ptr<IComponentStore>
 * in a FastHashMap<TypeId, ...>. This interface provides the operations
 * that don't require knowing the component type T:
 * - Existence checks (has)
 * - Removal (remove)
 * - Size/empty queries
 * - Entity count
 */
class IComponentStore
{
public:
    virtual ~IComponentStore() = default;

    /// @brief Returns true if this store contains a component for entity.
    [[nodiscard]] virtual bool has(Entity entity) const noexcept = 0;

    /// @brief Removes the component for entity. Returns true if removed.
    virtual bool remove(Entity entity) = 0;

    /// @brief Returns the number of components in this store.
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;

    /// @brief Returns true if this store has no components.
    [[nodiscard]] virtual bool empty() const noexcept = 0;

    /// @brief Removes all components from this store.
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
 * @brief Typed component storage backed by SparseSetWithData.
 *
 * @tparam T The component data type.
 *
 * @details
 * Wraps SparseSetWithData<uint32_t, T> with an Entity-typed API.
 * EntityTraits::toIndex() converts Entity to the uint32_t that
 * SparseSet requires internally.
 *
 * The dense arrays (entities and data) are stored contiguously,
 * so iteration over all components of type T is cache-friendly.
 *
 * @note Exception Safety:
 * - add/emplace: Strong guarantee (SparseSetWithData provides this).
 * - remove: Basic guarantee (swap-with-back may move-assign T).
 * - get/tryGet: No-throw.
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
        return mStorage.erase(EntityTraits::toIndex(entity));
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
     * @brief Adds a component for entity. Returns true if added.
     *
     * @param entity The entity to add a component for.
     * @param component The component data (copied).
     * @return true if the component was added; false if entity already had one.
     */
    bool add(Entity entity, const T& component)
    {
        return mStorage.insert(EntityTraits::toIndex(entity), component);
    }

    /**
     * @brief Adds a component for entity (move). Returns true if added.
     */
    bool add(Entity entity, T&& component)
    {
        return mStorage.insert(EntityTraits::toIndex(entity), std::move(component));
    }

    /**
     * @brief Constructs a component in-place for entity.
     *
     * @tparam Args Constructor argument types for T.
     * @param entity The entity to add a component for.
     * @param args Arguments forwarded to T's constructor.
     * @return true if the component was added; false if entity already had one.
     */
    template <typename... Args>
    bool emplace(Entity entity, Args&&... args)
    {
        return mStorage.emplace(EntityTraits::toIndex(entity), std::forward<Args>(args)...);
    }

    /**
     * @brief Returns a pointer to the component for entity, or nullptr.
     *
     * @param entity The entity to look up.
     * @return Pointer to component data, or nullptr if absent.
     *
     * @note Complexity: O(1).
     */
    [[nodiscard]] T* tryGet(Entity entity) noexcept
    {
        return mStorage.tryGet(EntityTraits::toIndex(entity));
    }

    [[nodiscard]] const T* tryGet(Entity entity) const noexcept
    {
        return mStorage.tryGet(EntityTraits::toIndex(entity));
    }

    /**
     * @brief Returns a reference to the component for entity.
     *
     * @param entity The entity to look up.
     * @return Reference to component data.
     * @throws std::out_of_range if entity has no component in this store.
     */
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

    /// @brief Returns the dense entity index array.
    [[nodiscard]] const std::vector<IndexType>& dense() const noexcept
    {
        return mStorage.dense();
    }

    /// @brief Returns the dense component data array.
    [[nodiscard]] const std::vector<T>& data() const noexcept
    {
        return mStorage.data();
    }

    /// @brief Returns a mutable reference to component at dense index.
    [[nodiscard]] T& dataAt(std::size_t denseIndex)
    {
        return mStorage.dataAt(denseIndex);
    }

    [[nodiscard]] const T& dataAt(std::size_t denseIndex) const
    {
        return mStorage.dataAt(denseIndex);
    }

    /// @brief Direct access to underlying SparseSetWithData.
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
