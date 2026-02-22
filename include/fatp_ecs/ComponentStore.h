#pragma once

/**
 * @file ComponentStore.h
 * @brief Type-erased component storage with pluggable storage policies.
 *
 * Three-layer type hierarchy:
 *
 *   IComponentStore              — fully type-erased (Registry map values)
 *   TypedIComponentStore<T>      — T-typed virtual interface (Registry typed ops)
 *   ComponentStore<T, Policy>    — concrete storage (direct View iteration)
 *
 * Registry holds IComponentStore* in its maps. Typed operations (add, get,
 * tryGet, emplace) go through TypedIComponentStore<T>* — one virtual call,
 * zero UB. View iteration pre-caches raw pointers from ComponentStore<T,P>*
 * via TypedIComponentStore<T>::typedStorePtr() — a single virtual call to
 * retrieve the concrete pointer, then all hot-loop iterations are unchecked
 * raw-pointer ops with no further virtual dispatch.
 *
 * Default policy (DefaultStoragePolicy) uses std::vector<T> — identical
 * behaviour and binary layout to the previous implementation. All existing
 * call sites work unchanged.
 *
 * FAT-P headers used:
 *   StoragePolicy.h — policy concept and built-in policies
 */

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fat_p/SparseSet.h>

#include "Entity.h"
#include "EventBus.h"
#include "StoragePolicy.h"

namespace fatp_ecs
{

// =============================================================================
// IComponentStore — Fully type-erased interface
// =============================================================================

class IComponentStore
{
public:
    virtual ~IComponentStore() = default;

    [[nodiscard]] virtual bool has(Entity entity) const noexcept = 0;
    virtual bool remove(Entity entity) = 0;
    virtual bool removeAndNotify(Entity entity, EventBus& events) = 0;

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool empty() const noexcept = 0;
    virtual void clear() = 0;

    [[nodiscard]] virtual const Entity* denseEntities() const noexcept = 0;
    [[nodiscard]] virtual std::size_t denseEntityCount() const noexcept = 0;

    virtual bool copyTo(Entity src, Entity dst, EventBus& events) = 0;

    IComponentStore() = default;
    IComponentStore(const IComponentStore&) = delete;
    IComponentStore& operator=(const IComponentStore&) = delete;
    IComponentStore(IComponentStore&&) = delete;
    IComponentStore& operator=(IComponentStore&&) = delete;
};

// =============================================================================
// TypedIComponentStore<T> — T-typed virtual interface
//
// Registry uses this to call typed operations without knowing the Policy.
// View uses typedStorePtr() + downcast-safe raw pointer accessors.
// =============================================================================

template <typename T>
class TypedIComponentStore : public IComponentStore
{
public:
    // Typed CRUD — one virtual dispatch per operation
    virtual T* addComponent(Entity entity, const T& v) = 0;
    virtual T* addComponent(Entity entity, T&& v) = 0;

    template <typename... Args>
    T* emplaceComponent(Entity entity, Args&&... args)
    {
        // Forward through a type-erased helper that each concrete class overrides.
        // We use placement-new style: construct into a temporary and move.
        return emplaceImpl(entity, T(std::forward<Args>(args)...));
    }

    virtual T* tryGetComponent(Entity entity) noexcept = 0;
    virtual const T* tryGetComponent(Entity entity) const noexcept = 0;
    virtual T& getComponent(Entity entity) = 0;
    virtual const T& getComponent(Entity entity) const = 0;

    // Raw-pointer accessors for View pre-caching (single virtual call,
    // then all hot-loop iterations are raw unchecked pointer arithmetic)
    [[nodiscard]] virtual const Entity*   densePtrTyped()      const noexcept = 0;
    [[nodiscard]] virtual std::size_t     denseCountTyped()    const noexcept = 0;
    [[nodiscard]] virtual const uint32_t* sparsePtrTyped()     const noexcept = 0;
    [[nodiscard]] virtual std::size_t     sparseCountTyped()   const noexcept = 0;
    [[nodiscard]] virtual T*              componentDataPtrTyped()    noexcept = 0;
    [[nodiscard]] virtual const T*        componentDataPtrTyped() const noexcept = 0;

    // Dense/data array accessors (for OwningGroup, sort, snapshot)
    [[nodiscard]] virtual const std::vector<Entity>& denseTyped()    const noexcept = 0;
    [[nodiscard]] virtual T&        dataAtTyped(std::size_t i) = 0;
    [[nodiscard]] virtual const T&  dataAtTyped(std::size_t i) const = 0;
    [[nodiscard]] virtual T&        dataAtUncheckedTyped(std::size_t i) noexcept = 0;
    [[nodiscard]] virtual const T&  dataAtUncheckedTyped(std::size_t i) const noexcept = 0;
    [[nodiscard]] virtual T&        getUncheckedTyped(Entity entity) noexcept = 0;
    [[nodiscard]] virtual const T&  getUncheckedTyped(Entity entity) const noexcept = 0;

    // Group support
    [[nodiscard]] virtual std::size_t getDenseIndexTyped(Entity entity) const noexcept = 0;
    virtual void swapDenseEntriesTyped(std::size_t i, std::size_t j) noexcept = 0;

    // Storage policy metadata
    [[nodiscard]] virtual std::size_t dataAlignmentTyped() const noexcept = 0;

    // =========================================================================
    // Non-virtual forwarders — same names as ComponentStore<T,P> methods
    //
    // These let View, OwningGroup, and sort helpers use TypedIComponentStore<T>*
    // with identical call syntax to the old ComponentStore<T>* interface.
    // One virtual dispatch per call (the *Typed() virtual); the hot loop inside
    // View operates on pre-cached raw pointers, so this is called once per view.
    // =========================================================================

    [[nodiscard]] const Entity*   densePtr()     const noexcept { return densePtrTyped(); }
    [[nodiscard]] std::size_t     denseCount()   const noexcept { return denseCountTyped(); }
    [[nodiscard]] const uint32_t* sparsePtr()    const noexcept { return sparsePtrTyped(); }
    [[nodiscard]] std::size_t     sparseCount()  const noexcept { return sparseCountTyped(); }
    [[nodiscard]] T*       componentDataPtr()          noexcept { return componentDataPtrTyped(); }
    [[nodiscard]] const T* componentDataPtr()    const noexcept { return componentDataPtrTyped(); }

    [[nodiscard]] const std::vector<Entity>& dense()         const noexcept { return denseTyped(); }

    [[nodiscard]] T&       dataAt(std::size_t i)                    { return dataAtTyped(i); }
    [[nodiscard]] const T& dataAt(std::size_t i)              const { return dataAtTyped(i); }
    [[nodiscard]] T&       dataAtUnchecked(std::size_t i)     noexcept { return dataAtUncheckedTyped(i); }
    [[nodiscard]] const T& dataAtUnchecked(std::size_t i) const noexcept { return dataAtUncheckedTyped(i); }

    [[nodiscard]] T&       getUnchecked(Entity e)       noexcept { return getUncheckedTyped(e); }
    [[nodiscard]] const T& getUnchecked(Entity e) const noexcept { return getUncheckedTyped(e); }

    [[nodiscard]] std::size_t getDenseIndex(Entity e) const noexcept { return getDenseIndexTyped(e); }
    void swapDenseEntries(std::size_t i, std::size_t j) noexcept { swapDenseEntriesTyped(i, j); }

    // Aliases for the typed CRUD (same names as ComponentStore<T,P> non-virtual methods)
    T* tryGet(Entity e) noexcept        { return tryGetComponent(e); }
    const T* tryGet(Entity e) const noexcept { return tryGetComponent(e); }
    T& get(Entity e)                    { return getComponent(e); }
    const T& get(Entity e) const        { return getComponent(e); }

    template <typename... Args>
    T* emplace(Entity e, Args&&... args) { return emplaceComponent(e, std::forward<Args>(args)...); }

    T* add(Entity e, const T& v) { return addComponent(e, v); }
    T* add(Entity e, T&& v)      { return addComponent(e, std::move(v)); }

protected:
    // Internal emplace helper — takes an already-constructed T by move
    virtual T* emplaceImpl(Entity entity, T&& v) = 0;
};

// =============================================================================
// ComponentStore<T, Policy> — Concrete storage
// =============================================================================

/**
 * @brief Typed component storage with pluggable data container policy.
 *
 * Backed by fat_p::SparseSetWithData<Entity, T, EntityIndex, StorageContainer>
 * where StorageContainer<U> = Policy<U>::container_type.
 *
 * The three-layer hierarchy (IComponentStore / TypedIComponentStore<T> /
 * ComponentStore<T,P>) is preserved so Registry dispatch, View, and
 * OwningGroup all work unchanged.
 */
template <typename T, template <typename> class Policy = DefaultStoragePolicy>
    requires StoragePolicy<Policy>
class ComponentStore : public TypedIComponentStore<T>
{
    // Alias template so Policy's container_type can be passed as DataContainer
    // to SparseSetWithData (C++20 alias templates work as template-template args).
    template <typename U>
    using StorageContainer = typename Policy<U>::container_type;

    using StorageType = fat_p::SparseSetWithData<Entity, T, EntityIndex, StorageContainer>;

public:
    using DataType      = T;
    using PolicyType    = Policy<T>;
    using ContainerType = typename PolicyType::container_type;

    ComponentStore() = default;

    // =========================================================================
    // IComponentStore — type-erased interface
    // =========================================================================

    [[nodiscard]] bool has(Entity entity) const noexcept override
    {
        return mStorage.contains(entity);
    }

    bool remove(Entity entity) override { return mStorage.erase(entity); }

    bool removeAndNotify(Entity entity, EventBus& events) override
    {
        if (!mStorage.contains(entity)) return false;
        events.emitComponentRemoved<T>(entity);
        return mStorage.erase(entity);
    }

    [[nodiscard]] std::size_t size() const noexcept override { return mStorage.size(); }
    [[nodiscard]] bool empty() const noexcept override { return mStorage.empty(); }

    void clear() override { mStorage.clear(); }

    [[nodiscard]] const Entity* denseEntities() const noexcept override { return mStorage.dense().data(); }
    [[nodiscard]] std::size_t denseEntityCount() const noexcept override { return mStorage.size(); }

    bool copyTo(Entity src, Entity dst, EventBus& events) override
    {
        if constexpr (!std::copyable<T>)
        {
            (void)src; (void)dst; (void)events;
            return false;
        }
        else
        {
            const T* s = mStorage.tryGet(src);
            if (!s) return false;
            T* d = mStorage.tryGet(dst);
            if (d) { *d = *s; events.emitComponentUpdated<T>(dst, *d); }
            else
            {
                T* ins = mStorage.tryEmplace(dst, *s);
                if (ins) events.emitComponentAdded<T>(dst, *ins);
            }
            return true;
        }
    }

    // =========================================================================
    // TypedIComponentStore<T> — T-typed virtual interface
    // =========================================================================

    T* addComponent(Entity entity, const T& v) override
    {
        if constexpr (std::copyable<T>)
            return mStorage.tryEmplace(entity, v);
        else
            return nullptr;
    }

    T* addComponent(Entity entity, T&& v)      override { return mStorage.tryEmplace(entity, std::move(v)); }

    T* tryGetComponent(Entity entity) noexcept override        { return mStorage.tryGet(entity); }
    const T* tryGetComponent(Entity entity) const noexcept override { return mStorage.tryGet(entity); }

    T& getComponent(Entity entity) override
    {
        T* p = mStorage.tryGet(entity);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    const T& getComponent(Entity entity) const override
    {
        const T* p = mStorage.tryGet(entity);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    [[nodiscard]] const Entity*   densePtrTyped()    const noexcept override { return mStorage.dense().data(); }
    [[nodiscard]] std::size_t     denseCountTyped()  const noexcept override { return mStorage.size(); }
    [[nodiscard]] const uint32_t* sparsePtrTyped()   const noexcept override { return mStorage.sparse().data(); }
    [[nodiscard]] std::size_t     sparseCountTyped() const noexcept override { return mStorage.sparse().size(); }
    [[nodiscard]] T*       componentDataPtrTyped()       noexcept override { return mStorage.empty() ? nullptr : &mStorage.dataAtUnchecked(0); }
    [[nodiscard]] const T* componentDataPtrTyped() const noexcept override { return mStorage.empty() ? nullptr : &mStorage.dataAtUnchecked(0); }

    [[nodiscard]] const std::vector<Entity>& denseTyped() const noexcept override { return mStorage.dense(); }

    [[nodiscard]] T&       dataAtTyped(std::size_t i)       override { return mStorage.dataAtUnchecked(i); }
    [[nodiscard]] const T& dataAtTyped(std::size_t i) const override { return mStorage.dataAtUnchecked(i); }

    [[nodiscard]] T&       dataAtUncheckedTyped(std::size_t i) noexcept override { return mStorage.dataAtUnchecked(i); }
    [[nodiscard]] const T& dataAtUncheckedTyped(std::size_t i) const noexcept override { return mStorage.dataAtUnchecked(i); }

    [[nodiscard]] T& getUncheckedTyped(Entity entity) noexcept override
    {
        return mStorage.dataAtUnchecked(mStorage.indexOf(entity));
    }

    [[nodiscard]] const T& getUncheckedTyped(Entity entity) const noexcept override
    {
        return mStorage.dataAtUnchecked(mStorage.indexOf(entity));
    }

    [[nodiscard]] std::size_t getDenseIndexTyped(Entity entity) const noexcept override
    {
        return mStorage.indexOf(entity);
    }

    void swapDenseEntriesTyped(std::size_t i, std::size_t j) noexcept override
    {
        swapDenseEntries(i, j);
    }

    [[nodiscard]] std::size_t dataAlignmentTyped() const noexcept override
    {
        return dataAlignment();
    }

    // =========================================================================
    // Non-virtual typed interface (used by View after downcast via typedPtr())
    // =========================================================================

    T* add(Entity e, const T& v)  { return addComponent(e, v); }
    T* add(Entity e, T&& v)       { return addComponent(e, std::move(v)); }

    template <typename... Args>
    T* emplace(Entity e, Args&&... args)
    {
        return mStorage.tryEmplace(e, std::forward<Args>(args)...);
    }

    [[nodiscard]] T* tryGet(Entity e) noexcept             { return mStorage.tryGet(e); }
    [[nodiscard]] const T* tryGet(Entity e) const noexcept { return mStorage.tryGet(e); }

    [[nodiscard]] T& get(Entity e)
    {
        T* p = mStorage.tryGet(e);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    [[nodiscard]] const T& get(Entity e) const
    {
        const T* p = mStorage.tryGet(e);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    [[nodiscard]] const std::vector<Entity>& dense() const noexcept { return mStorage.dense(); }

    [[nodiscard]] T&       dataAt(std::size_t i)       { return mStorage.dataAtUnchecked(i); }
    [[nodiscard]] const T& dataAt(std::size_t i) const { return mStorage.dataAtUnchecked(i); }
    [[nodiscard]] T&       dataAtUnchecked(std::size_t i) noexcept       { return mStorage.dataAtUnchecked(i); }
    [[nodiscard]] const T& dataAtUnchecked(std::size_t i) const noexcept { return mStorage.dataAtUnchecked(i); }

    [[nodiscard]] T& getUnchecked(Entity e) noexcept
    {
        return mStorage.dataAtUnchecked(mStorage.indexOf(e));
    }

    [[nodiscard]] const T& getUnchecked(Entity e) const noexcept
    {
        return mStorage.dataAtUnchecked(mStorage.indexOf(e));
    }

    [[nodiscard]] const Entity*   densePtr()    const noexcept { return mStorage.dense().data(); }
    [[nodiscard]] std::size_t     denseCount()  const noexcept { return mStorage.size(); }
    [[nodiscard]] const uint32_t* sparsePtr()   const noexcept { return mStorage.sparse().data(); }
    [[nodiscard]] std::size_t     sparseCount() const noexcept { return mStorage.sparse().size(); }
    [[nodiscard]] T*       componentDataPtr()       noexcept { return mStorage.empty() ? nullptr : &mStorage.dataAtUnchecked(0); }
    [[nodiscard]] const T* componentDataPtr() const noexcept { return mStorage.empty() ? nullptr : &mStorage.dataAtUnchecked(0); }

    [[nodiscard]] std::size_t getDenseIndex(Entity e) const noexcept { return mStorage.indexOf(e); }

    void swapDenseEntries(std::size_t i, std::size_t j) noexcept
    {
        mStorage.swapDenseEntries(i, j);
    }

    static constexpr std::size_t dataAlignment() noexcept
    {
        if constexpr (requires { ContainerType::alignment; })
            return ContainerType::alignment;
        else
            return alignof(T);
    }

protected:
    // TypedIComponentStore<T> pure virtual — forward to mStorage
    T* emplaceImpl(Entity entity, T&& v) override
    {
        return mStorage.tryEmplace(entity, std::move(v));
    }

private:
    // =========================================================================
    // Data members
    // =========================================================================

    StorageType mStorage;
};

} // namespace fatp_ecs
