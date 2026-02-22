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
    [[nodiscard]] virtual std::vector<Entity>&        mutableDenseTyped() noexcept = 0;
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
    [[nodiscard]] std::vector<Entity>&        mutableDense()       noexcept { return mutableDenseTyped(); }

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
 * Three parallel arrays:
 *   mSparse[slotIndex] = dense index (kTombstone = absent)
 *   mDense[denseIndex] = Entity
 *   mData[denseIndex]  = T  (policy-defined container)
 */
template <typename T, template <typename> class Policy = DefaultStoragePolicy>
    requires StoragePolicy<Policy>
class ComponentStore : public TypedIComponentStore<T>
{
public:
    using DataType      = T;
    using PolicyType    = Policy<T>;
    using ContainerType = typename PolicyType::container_type;

    static constexpr uint32_t kTombstone = std::numeric_limits<uint32_t>::max();

    ComponentStore() : mData(PolicyType::make()) {}

    // =========================================================================
    // IComponentStore — type-erased interface
    // =========================================================================

    [[nodiscard]] bool has(Entity entity) const noexcept override
    {
        return denseIndexOf(entity) < mDense.size();
    }

    bool remove(Entity entity) override { return eraseImpl(entity); }

    bool removeAndNotify(Entity entity, EventBus& events) override
    {
        if (!has(entity)) return false;
        events.emitComponentRemoved<T>(entity);
        return eraseImpl(entity);
    }

    [[nodiscard]] std::size_t size() const noexcept override { return mDense.size(); }
    [[nodiscard]] bool empty() const noexcept override { return mDense.empty(); }

    void clear() override
    {
        while (!mDense.empty()) eraseImpl(mDense.back());
    }

    [[nodiscard]] const Entity* denseEntities() const noexcept override { return mDense.data(); }
    [[nodiscard]] std::size_t denseEntityCount() const noexcept override { return mDense.size(); }

    bool copyTo(Entity src, Entity dst, EventBus& events) override
    {
        if constexpr (!std::copyable<T>)
        {
            (void)src; (void)dst; (void)events;
            return false;
        }
        else
        {
            const T* s = tryGetRaw(src);
            if (!s) return false;
            T* d = tryGetRaw(dst);
            if (d) { *d = *s; events.emitComponentUpdated<T>(dst, *d); }
            else
            {
                T* ins = insertImpl(dst, *s);
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
            return insertImpl(entity, T(v));
        else
            return nullptr;  // unreachable for non-copyable T
    }

    T* addComponent(Entity entity, T&& v)      override { return insertImpl(entity, std::move(v)); }

    T* tryGetComponent(Entity entity) noexcept override        { return tryGetRaw(entity); }
    const T* tryGetComponent(Entity entity) const noexcept override { return tryGetRaw(entity); }

    T& getComponent(Entity entity) override
    {
        T* p = tryGetRaw(entity);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    const T& getComponent(Entity entity) const override
    {
        const T* p = tryGetRaw(entity);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    [[nodiscard]] const Entity*   densePtrTyped()    const noexcept override { return mDense.data(); }
    [[nodiscard]] std::size_t     denseCountTyped()  const noexcept override { return mDense.size(); }
    [[nodiscard]] const uint32_t* sparsePtrTyped()   const noexcept override { return mSparse.data(); }
    [[nodiscard]] std::size_t     sparseCountTyped() const noexcept override { return mSparse.size(); }
    [[nodiscard]] T*       componentDataPtrTyped()       noexcept override { return mData.data(); }
    [[nodiscard]] const T* componentDataPtrTyped() const noexcept override { return mData.data(); }

    [[nodiscard]] const std::vector<Entity>& denseTyped() const noexcept override { return mDense; }
    [[nodiscard]] std::vector<Entity>& mutableDenseTyped() noexcept override { return mDense; }

    [[nodiscard]] T&       dataAtTyped(std::size_t i)       override { return mData[i]; }
    [[nodiscard]] const T& dataAtTyped(std::size_t i) const override { return mData[i]; }

    [[nodiscard]] T&       dataAtUncheckedTyped(std::size_t i) noexcept override { return mData[i]; }
    [[nodiscard]] const T& dataAtUncheckedTyped(std::size_t i) const noexcept override { return mData[i]; }

    [[nodiscard]] T& getUncheckedTyped(Entity entity) noexcept override
    {
        return mData[mSparse[EntityIndex::index(entity)]];
    }

    [[nodiscard]] const T& getUncheckedTyped(Entity entity) const noexcept override
    {
        return mData[mSparse[EntityIndex::index(entity)]];
    }

    [[nodiscard]] std::size_t getDenseIndexTyped(Entity entity) const noexcept override
    {
        return denseIndexOf(entity);
    }

    void swapDenseEntriesTyped(std::size_t i, std::size_t j) noexcept override
    {
        if (i == j) return;
        std::swap(mDense[i], mDense[j]);
        std::swap(mData[i],  mData[j]);
        mSparse[EntityIndex::index(mDense[i])] = static_cast<uint32_t>(i);
        mSparse[EntityIndex::index(mDense[j])] = static_cast<uint32_t>(j);
    }

    [[nodiscard]] std::size_t dataAlignmentTyped() const noexcept override
    {
        return dataAlignment();
    }

    // =========================================================================
    // Non-virtual typed interface (used by View after downcast via typedPtr())
    // These mirror TypedIComponentStore virtuals but are non-virtual for
    // use in templates where the concrete type is known at compile time.
    // =========================================================================

    // The legacy non-virtual names — used by View, OwningGroup, sort helpers.
    // These are NOT virtual; they're called through ComponentStore<T,P>* directly
    // after a safe downcast from TypedIComponentStore<T>*.

    T* add(Entity e, const T& v)  { return insertImpl(e, v); }
    T* add(Entity e, T&& v)       { return insertImpl(e, std::move(v)); }

    template <typename... Args>
    T* emplace(Entity e, Args&&... args)
    {
        return insertImpl(e, T(std::forward<Args>(args)...));
    }

    [[nodiscard]] T* tryGet(Entity e) noexcept             { return tryGetRaw(e); }
    [[nodiscard]] const T* tryGet(Entity e) const noexcept { return tryGetRaw(e); }

    [[nodiscard]] T& get(Entity e)
    {
        T* p = tryGetRaw(e);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    [[nodiscard]] const T& get(Entity e) const
    {
        const T* p = tryGetRaw(e);
        if (!p) throw std::out_of_range("ComponentStore::get: entity not found");
        return *p;
    }

    [[nodiscard]] const std::vector<Entity>& dense() const noexcept { return mDense; }
    [[nodiscard]] std::vector<Entity>& mutableDense() noexcept { return mDense; }

    [[nodiscard]] T&       dataAt(std::size_t i)       { return mData[i]; }
    [[nodiscard]] const T& dataAt(std::size_t i) const { return mData[i]; }
    [[nodiscard]] T&       dataAtUnchecked(std::size_t i) noexcept       { return mData[i]; }
    [[nodiscard]] const T& dataAtUnchecked(std::size_t i) const noexcept { return mData[i]; }

    [[nodiscard]] T& getUnchecked(Entity e) noexcept
    {
        return mData[mSparse[EntityIndex::index(e)]];
    }

    [[nodiscard]] const T& getUnchecked(Entity e) const noexcept
    {
        return mData[mSparse[EntityIndex::index(e)]];
    }

    [[nodiscard]] const Entity*   densePtr()    const noexcept { return mDense.data(); }
    [[nodiscard]] std::size_t     denseCount()  const noexcept { return mDense.size(); }
    [[nodiscard]] const uint32_t* sparsePtr()   const noexcept { return mSparse.data(); }
    [[nodiscard]] std::size_t     sparseCount() const noexcept { return mSparse.size(); }
    [[nodiscard]] T*       componentDataPtr()       noexcept { return mData.data(); }
    [[nodiscard]] const T* componentDataPtr() const noexcept { return mData.data(); }

    [[nodiscard]] std::size_t getDenseIndex(Entity e) const noexcept { return denseIndexOf(e); }

    void swapDenseEntries(std::size_t i, std::size_t j) noexcept
    {
        if (i == j) return;
        std::swap(mDense[i], mDense[j]);
        std::swap(mData[i],  mData[j]);
        mSparse[EntityIndex::index(mDense[i])] = static_cast<uint32_t>(i);
        mSparse[EntityIndex::index(mDense[j])] = static_cast<uint32_t>(j);
    }

    static constexpr std::size_t dataAlignment() noexcept
    {
        if constexpr (requires { ContainerType::alignment; })
            return ContainerType::alignment;
        else
            return alignof(T);
    }

protected:
    // TypedIComponentStore<T> pure virtual — forward to insertImpl
    T* emplaceImpl(Entity entity, T&& v) override
    {
        return insertImpl(entity, std::move(v));
    }

private:
    // =========================================================================
    // Core helpers
    // =========================================================================

    [[nodiscard]] std::size_t denseIndexOf(Entity entity) const noexcept
    {
        const auto slot = static_cast<std::size_t>(EntityIndex::index(entity));
        if (slot >= mSparse.size()) return mDense.size();
        const std::size_t di = mSparse[slot];
        if (di >= mDense.size() || EntityIndex::index(mDense[di]) != slot)
            return mDense.size();
        return di;
    }

    [[nodiscard]] T* tryGetRaw(Entity entity) noexcept
    {
        const std::size_t di = denseIndexOf(entity);
        return di < mDense.size() ? &mData[di] : nullptr;
    }

    [[nodiscard]] const T* tryGetRaw(Entity entity) const noexcept
    {
        const std::size_t di = denseIndexOf(entity);
        return di < mDense.size() ? &mData[di] : nullptr;
    }

    template <typename U>
    T* insertImpl(Entity entity, U&& value)
    {
        const auto slot = static_cast<std::size_t>(EntityIndex::index(entity));

        if (slot >= mSparse.size())
            mSparse.resize(slot + 1, kTombstone);

        const std::size_t existing = mSparse[slot];
        if (existing < mDense.size() && EntityIndex::index(mDense[existing]) == slot)
            return nullptr; // already present

        const std::size_t di = mDense.size();
        mDense.push_back(entity);
        mData.push_back(std::forward<U>(value));
        mSparse[slot] = static_cast<uint32_t>(di);
        return &mData[di];
    }

    bool eraseImpl(Entity entity) noexcept
    {
        const auto slot = static_cast<std::size_t>(EntityIndex::index(entity));
        if (slot >= mSparse.size()) return false;

        const std::size_t di = mSparse[slot];
        if (di >= mDense.size() || EntityIndex::index(mDense[di]) != slot)
            return false;

        const std::size_t last = mDense.size() - 1;
        if (di != last)
        {
            const Entity lastEntity = mDense[last];
            mDense[di] = lastEntity;
            mData[di]  = std::move(mData[last]);
            mSparse[EntityIndex::index(lastEntity)] = static_cast<uint32_t>(di);
        }

        mDense.pop_back();
        mData.pop_back();
        mSparse[slot] = kTombstone;
        return true;
    }

    // =========================================================================
    // Data members
    // =========================================================================

    std::vector<uint32_t> mSparse;
    std::vector<Entity>   mDense;
    ContainerType         mData;
};

} // namespace fatp_ecs
