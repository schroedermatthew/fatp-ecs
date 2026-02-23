#pragma once

/**
 * @file View.h
 * @brief Component view for iterating entities with specific component sets.
 */

// FAT-P components used (indirectly via ComponentStore):
// - SparseSetWithData: Dense array iteration for cache-friendly access
//   - EntityIndex policy: Dense array stores full 64-bit Entity values
//
// Views iterate the smallest ComponentStore and probe the others for
// intersection (smallest-set intersection strategy). Entity handles
// are read from ComponentStore::densePtr() which provides full 64-bit
// Entity values with generation (via EntityIndex policy), enabling
// safe use of the entities for destroy(), isAlive(), and command
// buffer operations.
//
// Exclude filters:
//
// View<A, B> can be constructed with an optional Exclude<Xs...> argument.
// Entities that possess any excluded component type are skipped during
// iteration. Exclude stores are nullable — if a type has never been
// registered in the Registry (no entity ever had that component), the
// store pointer is null, which is treated as "no entity has it" and the
// exclude check trivially passes. This matches EnTT's behaviour.
//
// The exclude check is integrated into the per-entity gate in
// invokeFuncIfPresent, adding one sparse array read per excluded type
// per entity (same cost as a non-pivot include probe).
//
// Performance design — Clang aliasing fix:
//
// The previous implementation accessed non-pivot components via has() then
// getUnchecked() inside the hot loop. Both methods inline into reads of
// SparseSetWithData's internal std::vector metadata (data pointer, size).
// Clang's alias analysis concluded these fields could be modified by the
// loop body (volatile gSink writes, mutable component references), so it
// reloaded the vector metadata from memory on every iteration rather than
// hoisting it to registers. This generated 8-10 extra loads per iteration
// in the 2-comp and 3-comp cases, causing the regression observed on
// Clang-16/17 in CI benchmarks.
//
// The fix: eachWithPivot pre-caches raw pointers from every non-pivot
// ComponentStore into local variables before the loop. Stack-local pointer
// variables are provably unaliased from any store's internal data, so Clang
// (and GCC) can keep them in registers for the duration of the loop with
// no per-iteration reloads.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>

#include "ComponentStore.h"
#include "Entity.h"

namespace fatp_ecs
{

class Registry;

// =============================================================================
// Exclude - Tag type for exclude filters
// =============================================================================

/**
 * @brief Tag type expressing component types that must be absent for an entity
 *        to be visited by a View.
 *
 * Usage:
 * @code
 *   // Iterate entities with Position and Velocity, skipping those with Frozen.
 *   registry.view<Position, Velocity>(Exclude<Frozen>{}).each(...);
 *
 *   // Multiple excluded types:
 *   registry.view<Position>(Exclude<Frozen, Dead>{}).each(...);
 * @endcode
 *
 * @tparam Xs Component types that disqualify an entity from iteration.
 */
template <typename... Xs>
struct Exclude
{
};

// =============================================================================
// ViewImpl - internal implementation parameterised on include and exclude packs
// =============================================================================

template <typename IncludePack, typename ExcludePack>
class ViewImpl;

template <typename... Ts, typename... Xs>
class ViewImpl<std::tuple<Ts...>, std::tuple<Xs...>>
{
    static_assert(sizeof...(Ts) > 0, "View requires at least one component type");

public:
    // Include-only constructor (no exclusions).
    explicit ViewImpl(TypedIComponentStore<Ts>*... includeStores)
        : mStores(includeStores...)
        , mExcludeStores()
    {
    }

    // Include + exclude constructor.
    // Uses a struct tag to avoid overload ambiguity when Xs is empty.
    struct WithExclude {};
    explicit ViewImpl(WithExclude,
                      std::tuple<TypedIComponentStore<Ts>*...> includeStores,
                      std::tuple<TypedIComponentStore<Xs>*...> excludeStores)
        : mStores(std::move(includeStores))
        , mExcludeStores(std::move(excludeStores))
    {
    }

    // =========================================================================
    // Iteration
    // =========================================================================

    template <typename Func>
    void each(Func&& func)
    {
        if (anyStoreNull())
        {
            return;
        }

        if constexpr (sizeof...(Ts) == 1)
        {
            eachSingleComponent(std::forward<Func>(func));
        }
        else
        {
            eachMultiComponent(std::forward<Func>(func));
        }
    }

    template <typename Func>
    void each(Func&& func) const
    {
        if (anyStoreNull())
        {
            return;
        }

        if constexpr (sizeof...(Ts) == 1)
        {
            eachSingleComponentConst(std::forward<Func>(func));
        }
        else
        {
            eachMultiComponentConst(std::forward<Func>(func));
        }
    }

    [[nodiscard]] std::size_t count() const
    {
        if constexpr (sizeof...(Ts) == 1 && sizeof...(Xs) == 0)
        {
            // Fast path: single include, no exclusions — size() is exact.
            return anyStoreNull() ? 0 : std::get<0>(mStores)->size();
        }
        else
        {
            if (anyStoreNull())
            {
                return 0;
            }
            std::size_t result = 0;
            if constexpr (sizeof...(Ts) == 1)
            {
                eachSingleComponentConst(
                    [&result](Entity, const Ts&...) { ++result; });
            }
            else
            {
                eachMultiComponentConst(
                    [&result](Entity, const Ts&...) { ++result; });
            }
            return result;
        }
    }

private:
    std::tuple<TypedIComponentStore<Ts>*...> mStores;
    std::tuple<TypedIComponentStore<Xs>*...> mExcludeStores;

    // =========================================================================
    // Null check (include stores only)
    // =========================================================================

    [[nodiscard]] bool anyStoreNull() const noexcept
    {
        return anyStoreNullImpl(std::index_sequence_for<Ts...>{});
    }

    template <std::size_t... Is>
    [[nodiscard]] bool anyStoreNullImpl(std::index_sequence<Is...>) const noexcept
    {
        return ((std::get<Is>(mStores) == nullptr) || ...);
    }

    // =========================================================================
    // Exclude check — pre-cached raw pointer bundle
    //
    // A null exclude store means the type was never registered; no entity has
    // it, so the check trivially passes (entity is not excluded).
    // =========================================================================

    struct ExcludeCache
    {
        const uint32_t* sparseData;
        std::size_t     sparseSize;
        const Entity*   denseData;
        std::size_t     denseSize;
        bool            isNull;

        ExcludeCache() noexcept
            : sparseData(nullptr), sparseSize(0)
            , denseData(nullptr),  denseSize(0)
            , isNull(true)
        {
        }

        template <typename X>
        explicit ExcludeCache(const TypedIComponentStore<X>* store) noexcept
            : sparseData(store ? store->sparsePtr()   : nullptr)
            , sparseSize(store ? store->sparseCount() : 0)
            , denseData (store ? store->densePtr()    : nullptr)
            , denseSize (store ? store->denseCount()  : 0)
            , isNull(store == nullptr)
        {
        }

        // Returns true if entity IS present (entity should be excluded).
        [[nodiscard]] bool has(Entity entity) const noexcept
        {
            if (isNull) return false;
            const uint32_t sparseIdx = EntityIndex::index(entity);
            if (sparseIdx >= sparseSize) return false;
            const uint32_t denseIdx = sparseData[sparseIdx];
            if (denseIdx >= denseSize) return false;
            return denseData[denseIdx] == entity;
        }
    };

    [[nodiscard]] auto buildExcludeCaches() const
    {
        return buildExcludeCachesImpl(std::index_sequence_for<Xs...>{});
    }

    template <std::size_t... Is>
    [[nodiscard]] auto buildExcludeCachesImpl(std::index_sequence<Is...>) const
    {
        return std::make_tuple(
            ExcludeCache(std::get<Is>(mExcludeStores))...);
    }

    template <typename ExcludeCaches>
    [[nodiscard]] static bool entityIsExcludedCached(
        Entity entity, const ExcludeCaches& caches) noexcept
    {
        return entityIsExcludedCachedImpl(
            entity, caches, std::index_sequence_for<Xs...>{});
    }

    template <typename ExcludeCaches, std::size_t... Is>
    [[nodiscard]] static bool entityIsExcludedCachedImpl(
        Entity entity, const ExcludeCaches& caches,
        std::index_sequence<Is...>) noexcept
    {
        return (std::get<Is>(caches).has(entity) || ...);
    }

    // =========================================================================
    // Single-component iteration
    // =========================================================================

    template <typename Func>
    void eachSingleComponent(Func&& func)
    {
        using T0 = std::tuple_element_t<0, std::tuple<Ts...>>;
        auto*             store = std::get<0>(mStores);
        const Entity*     ents  = store->densePtr();
        const std::size_t cnt   = store->size();
        // Cache the raw component pointer once — eliminates the virtual
        // dispatch that store->dataAt(i) would incur on every iteration.
        T0*               data  = store->componentDataPtr();

        if constexpr (sizeof...(Xs) == 0)
        {
            for (std::size_t i = 0; i < cnt; ++i)
            {
                func(ents[i], data[i]);
            }
        }
        else
        {
            auto excCaches = buildExcludeCaches();
            for (std::size_t i = 0; i < cnt; ++i)
            {
                Entity entity = ents[i];
                if (entityIsExcludedCached(entity, excCaches)) continue;
                func(entity, data[i]);
            }
        }
    }

    template <typename Func>
    void eachSingleComponentConst(Func&& func) const
    {
        using T0 = std::tuple_element_t<0, std::tuple<Ts...>>;
        const auto*       store = std::get<0>(mStores);
        const Entity*     ents  = store->densePtr();
        const std::size_t cnt   = store->size();
        // Cache the raw component pointer once — eliminates the virtual
        // dispatch that store->dataAt(i) would incur on every iteration.
        const T0*         data  = store->componentDataPtr();

        if constexpr (sizeof...(Xs) == 0)
        {
            for (std::size_t i = 0; i < cnt; ++i)
            {
                func(ents[i], data[i]);
            }
        }
        else
        {
            auto excCaches = buildExcludeCaches();
            for (std::size_t i = 0; i < cnt; ++i)
            {
                Entity entity = ents[i];
                if (entityIsExcludedCached(entity, excCaches)) continue;
                func(entity, data[i]);
            }
        }
    }

    // =========================================================================
    // Multi-component iteration (smallest-set intersection)
    // =========================================================================

    [[nodiscard]] std::size_t findSmallestStoreIndex() const
    {
        return findSmallestImpl(std::index_sequence_for<Ts...>{});
    }

    template <std::size_t... Is>
    [[nodiscard]] std::size_t findSmallestImpl(std::index_sequence<Is...>) const
    {
        std::size_t sizes[] = {std::get<Is>(mStores)->size()...};
        std::size_t minIdx  = 0;
        for (std::size_t i = 1; i < sizeof...(Ts); ++i)
        {
            if (sizes[i] < sizes[minIdx]) minIdx = i;
        }
        return minIdx;
    }

    template <typename Func>
    void eachMultiComponent(Func&& func)
    {
        if (allStoresSameSize())
        {
            eachWithPivot<0>(std::forward<Func>(func));
            return;
        }
        const std::size_t pivotIdx = findSmallestStoreIndex();
        eachWithPivotDispatch(std::forward<Func>(func), pivotIdx,
                              std::index_sequence_for<Ts...>{});
    }

    template <typename Func>
    void eachMultiComponentConst(Func&& func) const
    {
        if (allStoresSameSize())
        {
            eachWithPivotConst<0>(std::forward<Func>(func));
            return;
        }
        const std::size_t pivotIdx = findSmallestStoreIndex();
        eachWithPivotDispatchConst(std::forward<Func>(func), pivotIdx,
                                   std::index_sequence_for<Ts...>{});
    }

    [[nodiscard]] bool allStoresSameSize() const noexcept
    {
        return allStoresSameSizeImpl(std::index_sequence_for<Ts...>{});
    }

    template <std::size_t First, std::size_t... Rest>
    [[nodiscard]] bool allStoresSameSizeImpl(
        std::index_sequence<First, Rest...>) const noexcept
    {
        const std::size_t sz = std::get<First>(mStores)->size();
        return ((std::get<Rest>(mStores)->size() == sz) && ...);
    }

    template <typename Func, std::size_t... Is>
    void eachWithPivotDispatch(Func&& func, std::size_t pivotIdx,
                               std::index_sequence<Is...>)
    {
        (void)((pivotIdx == Is
              ? (eachWithPivot<Is>(std::forward<Func>(func)), true)
              : false) ||
         ...);
    }

    template <typename Func, std::size_t... Is>
    void eachWithPivotDispatchConst(Func&& func, std::size_t pivotIdx,
                                    std::index_sequence<Is...>) const
    {
        (void)((pivotIdx == Is
              ? (eachWithPivotConst<Is>(std::forward<Func>(func)), true)
              : false) ||
         ...);
    }

    // =========================================================================
    // Pre-caching raw pointer bundle for non-pivot include stores
    // =========================================================================

    template <std::size_t I>
    struct NonPivotCache
    {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        const uint32_t* sparseData;
        std::size_t     sparseSize;
        const Entity*   denseData;
        std::size_t     denseSize;
        T*              componentData;

        explicit NonPivotCache(TypedIComponentStore<T>* store) noexcept
            : sparseData(store->sparsePtr())
            , sparseSize(store->sparseCount())
            , denseData(store->densePtr())
            , denseSize(store->denseCount())
            , componentData(store->componentDataPtr())
        {
        }

        [[nodiscard]] T* tryGet(Entity entity) const noexcept
        {
            const uint32_t sparseIdx = EntityIndex::index(entity);
            if (sparseIdx >= sparseSize) return nullptr;
            const uint32_t denseIdx = sparseData[sparseIdx];
            if (denseIdx >= denseSize) return nullptr;
            if (denseData[denseIdx] != entity) return nullptr;
            return &componentData[denseIdx];
        }
    };

    template <std::size_t I>
    struct NonPivotCacheConst
    {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        const uint32_t* sparseData;
        std::size_t     sparseSize;
        const Entity*   denseData;
        std::size_t     denseSize;
        const T*        componentData;

        explicit NonPivotCacheConst(const TypedIComponentStore<T>* store) noexcept
            : sparseData(store->sparsePtr())
            , sparseSize(store->sparseCount())
            , denseData(store->densePtr())
            , denseSize(store->denseCount())
            , componentData(store->componentDataPtr())
        {
        }

        [[nodiscard]] const T* tryGet(Entity entity) const noexcept
        {
            const uint32_t sparseIdx = EntityIndex::index(entity);
            if (sparseIdx >= sparseSize) return nullptr;
            const uint32_t denseIdx = sparseData[sparseIdx];
            if (denseIdx >= denseSize) return nullptr;
            if (denseData[denseIdx] != entity) return nullptr;
            return &componentData[denseIdx];
        }
    };

    // =========================================================================
    // eachWithPivot - the hot loop
    // =========================================================================

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivot(Func&& func)
    {
        auto*             pivotStore = std::get<PivotIdx>(mStores);
        const Entity*     pivotEnts  = pivotStore->densePtr();
        const std::size_t cnt        = pivotStore->denseCount();
        using PivotT = std::tuple_element_t<PivotIdx, std::tuple<Ts...>>;
        PivotT*           pivotData  = pivotStore->componentDataPtr();

        auto caches    = buildCaches<PivotIdx>(std::index_sequence_for<Ts...>{});
        auto excCaches = buildExcludeCaches();

        for (std::size_t i = 0; i < cnt; ++i)
        {
            Entity entity = pivotEnts[i];
            if constexpr (sizeof...(Xs) > 0)
            {
                if (entityIsExcludedCached(entity, excCaches)) continue;
            }
            invokeFuncIfPresent<PivotIdx>(std::forward<Func>(func), entity, i,
                                          pivotData, caches,
                                          std::index_sequence_for<Ts...>{});
        }
    }

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivotConst(Func&& func) const
    {
        const auto*       pivotStore = std::get<PivotIdx>(mStores);
        const Entity*     pivotEnts  = pivotStore->densePtr();
        const std::size_t cnt        = pivotStore->denseCount();
        using PivotT = std::tuple_element_t<PivotIdx, std::tuple<Ts...>>;
        const PivotT*     pivotData  = pivotStore->componentDataPtr();

        auto caches    = buildCachesConst<PivotIdx>(std::index_sequence_for<Ts...>{});
        auto excCaches = buildExcludeCaches();

        for (std::size_t i = 0; i < cnt; ++i)
        {
            Entity entity = pivotEnts[i];
            if constexpr (sizeof...(Xs) > 0)
            {
                if (entityIsExcludedCached(entity, excCaches)) continue;
            }
            invokeFuncIfPresentConst<PivotIdx>(std::forward<Func>(func), entity, i,
                                               pivotData, caches,
                                               std::index_sequence_for<Ts...>{});
        }
    }

    // =========================================================================
    // Cache construction
    // =========================================================================

    template <std::size_t PivotIdx, std::size_t... Is>
    auto buildCaches(std::index_sequence<Is...>)
    {
        return std::make_tuple(buildCacheForIndex<PivotIdx, Is>()...);
    }

    template <std::size_t PivotIdx, std::size_t I>
    auto buildCacheForIndex()
    {
        if constexpr (I == PivotIdx)
            return std::nullptr_t{};
        else
            return NonPivotCache<I>(std::get<I>(mStores));
    }

    template <std::size_t PivotIdx, std::size_t... Is>
    auto buildCachesConst(std::index_sequence<Is...>) const
    {
        return std::make_tuple(buildCacheForIndexConst<PivotIdx, Is>()...);
    }

    template <std::size_t PivotIdx, std::size_t I>
    auto buildCacheForIndexConst() const
    {
        if constexpr (I == PivotIdx)
            return std::nullptr_t{};
        else
            return NonPivotCacheConst<I>(std::get<I>(mStores));
    }

    // =========================================================================
    // Per-entity combined lookup and invocation
    // =========================================================================

    template <std::size_t PivotIdx, typename Func, typename PivotT,
              typename Caches, std::size_t... Is>
    void invokeFuncIfPresent(Func&& func, Entity entity, std::size_t denseIdx,
                             PivotT* pivotData, Caches& caches,
                             std::index_sequence<Is...>)
    {
        auto ptrTuple = std::make_tuple(
            getPtr<PivotIdx, Is>(entity, denseIdx, pivotData, caches)...);

        if (!((Is == PivotIdx || std::get<Is>(ptrTuple) != nullptr) && ...))
            return;

        func(entity, *std::get<Is>(ptrTuple)...);
    }

    template <std::size_t PivotIdx, typename Func, typename PivotT,
              typename Caches, std::size_t... Is>
    void invokeFuncIfPresentConst(Func&& func, Entity entity, std::size_t denseIdx,
                                  const PivotT* pivotData, const Caches& caches,
                                  std::index_sequence<Is...>) const
    {
        auto ptrTuple = std::make_tuple(
            getPtrConst<PivotIdx, Is>(entity, denseIdx, pivotData, caches)...);

        if (!((Is == PivotIdx || std::get<Is>(ptrTuple) != nullptr) && ...))
            return;

        func(entity, *std::get<Is>(ptrTuple)...);
    }

    template <std::size_t PivotIdx, std::size_t I, typename PivotT, typename Caches>
    [[nodiscard]] auto* getPtr(Entity entity, std::size_t denseIdx,
                               PivotT* pivotData, Caches& caches) noexcept
    {
        using ElemT = std::tuple_element_t<I, std::tuple<Ts...>>;
        if constexpr (I == PivotIdx)
            return static_cast<ElemT*>(&pivotData[denseIdx]);
        else
            return std::get<I>(caches).tryGet(entity);
    }

    template <std::size_t PivotIdx, std::size_t I, typename PivotT, typename Caches>
    [[nodiscard]] const auto* getPtrConst(Entity entity, std::size_t denseIdx,
                                          const PivotT* pivotData,
                                          const Caches& caches) const noexcept
    {
        using ElemT = std::tuple_element_t<I, std::tuple<Ts...>>;
        if constexpr (I == PivotIdx)
            return static_cast<const ElemT*>(&pivotData[denseIdx]);
        else
            return std::get<I>(caches).tryGet(entity);
    }
};

// =============================================================================
// View - public alias (include-only, no exclusions)
// =============================================================================

template <typename... Ts>
using View = ViewImpl<std::tuple<Ts...>, std::tuple<>>;

} // namespace fatp_ecs
