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
// Performance design — Clang aliasing fix:
//
// The previous implementation accessed non-pivot components via has() then
// getUnchecked() inside the hot loop. Both methods inline into reads of
// SparseSetWithData's internal std::vector metadata (data pointer, size).
// Clang's alias analysis concluded these fields could be modified by the
// loop body (volatile gSink writes, mutable component references), so it
// reloaded the vector metadata from memory on every iteration rather than
// hoisting it to registers. This generated 8–10 extra loads per iteration
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
// View - Multi-Component Iteration
// =============================================================================

/**
 * @brief Iterates all entities possessing every component type in Ts.
 *
 * @tparam Ts Component types to match.
 *
 * @note Thread-safety: NOT thread-safe. Use Scheduler for parallel iteration.
 */
template <typename... Ts>
class View
{
    static_assert(sizeof...(Ts) > 0, "View requires at least one component type");

public:
    explicit View(ComponentStore<Ts>*... stores)
        : mStores(stores...)
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
        if (anyStoreNull())
        {
            return 0;
        }

        if constexpr (sizeof...(Ts) == 1)
        {
            return std::get<0>(mStores)->size();
        }
        else
        {
            std::size_t result = 0;
            eachMultiComponentConst([&result](Entity, const Ts&...) { ++result; });
            return result;
        }
    }

private:
    std::tuple<ComponentStore<Ts>*...> mStores;

    // =========================================================================
    // Null check
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
    // Single-component iteration
    // =========================================================================

    template <typename Func>
    void eachSingleComponent(Func&& func)
    {
        auto* store = std::get<0>(mStores);
        const Entity* ents = store->densePtr();
        const std::size_t cnt = store->size();

        for (std::size_t i = 0; i < cnt; ++i)
        {
            func(ents[i], store->dataAt(i));
        }
    }

    template <typename Func>
    void eachSingleComponentConst(Func&& func) const
    {
        const auto* store = std::get<0>(mStores);
        const Entity* ents = store->densePtr();
        const std::size_t cnt = store->size();

        for (std::size_t i = 0; i < cnt; ++i)
        {
            func(ents[i], store->dataAt(i));
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
        std::size_t minIdx = 0;
        for (std::size_t i = 1; i < sizeof...(Ts); ++i)
        {
            if (sizes[i] < sizes[minIdx])
            {
                minIdx = i;
            }
        }
        return minIdx;
    }

    template <typename Func>
    void eachMultiComponent(Func&& func)
    {
        // Fast path: all stores equal size — any pivot is equally good, always use 0.
        // Eliminates the runtime pivot dispatch fold-expression from the hot path,
        // which Clang struggled to optimize when sizes differ.
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
    // Pre-caching raw pointer bundle for non-pivot stores
    //
    // Loaded once before the loop. Clang/GCC keep these in registers because
    // they are stack-local values that cannot alias any heap-allocated store
    // data, eliminating the per-iteration metadata reloads of the previous
    // has() + getUnchecked() implementation.
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

        explicit NonPivotCache(ComponentStore<T>* store) noexcept
            : sparseData(store->sparsePtr())
            , sparseSize(store->sparseCount())
            , denseData(store->densePtr())
            , denseSize(store->denseCount())
            , componentData(store->componentDataPtr())
        {
        }

        // Returns pointer to component if entity is present, nullptr otherwise.
        // Single sparse array read — no second lookup needed.
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

    // Const version for const iteration paths
    template <std::size_t I>
    struct NonPivotCacheConst
    {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        const uint32_t* sparseData;
        std::size_t     sparseSize;
        const Entity*   denseData;
        std::size_t     denseSize;
        const T*        componentData;

        explicit NonPivotCacheConst(const ComponentStore<T>* store) noexcept
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
    // eachWithPivot — the hot loop
    //
    // For the pivot store: iterate its dense array with dataAtUnchecked(i).
    // For non-pivot stores: pre-cache raw pointers, probe via NonPivotCache.
    // =========================================================================

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivot(Func&& func)
    {
        auto* pivotStore = std::get<PivotIdx>(mStores);
        const Entity*   pivotEnts  = pivotStore->densePtr();
        const std::size_t cnt      = pivotStore->denseCount();
        using PivotT = std::tuple_element_t<PivotIdx, std::tuple<Ts...>>;
        PivotT* pivotData          = pivotStore->componentDataPtr();

        // Pre-cache raw pointers for all non-pivot stores (one load each, before loop).
        auto caches = buildCaches<PivotIdx>(std::index_sequence_for<Ts...>{});

        for (std::size_t i = 0; i < cnt; ++i)
        {
            Entity entity = pivotEnts[i];
            // Single-pass: tryGet() on each non-pivot cache, skip if any returns null.
            // This avoids the previous allPresent()+invokeFunc() double-tryGet pattern.
            invokeFuncIfPresent<PivotIdx>(std::forward<Func>(func), entity, i,
                                          pivotData, caches,
                                          std::index_sequence_for<Ts...>{});
        }
    }

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivotConst(Func&& func) const
    {
        const auto* pivotStore = std::get<PivotIdx>(mStores);
        const Entity*   pivotEnts  = pivotStore->densePtr();
        const std::size_t cnt      = pivotStore->denseCount();
        using PivotT = std::tuple_element_t<PivotIdx, std::tuple<Ts...>>;
        const PivotT* pivotData    = pivotStore->componentDataPtr();

        auto caches = buildCachesConst<PivotIdx>(std::index_sequence_for<Ts...>{});

        for (std::size_t i = 0; i < cnt; ++i)
        {
            Entity entity = pivotEnts[i];
            invokeFuncIfPresentConst<PivotIdx>(std::forward<Func>(func), entity, i,
                                               pivotData, caches,
                                               std::index_sequence_for<Ts...>{});
        }
    }

    // =========================================================================
    // Cache construction (before the loop)
    // =========================================================================

    // Build a tuple of NonPivotCache for each non-pivot index; nullptr_t for pivot.
    template <std::size_t PivotIdx, std::size_t... Is>
    auto buildCaches(std::index_sequence<Is...>)
    {
        return std::make_tuple(
            buildCacheForIndex<PivotIdx, Is>()...
        );
    }

    template <std::size_t PivotIdx, std::size_t I>
    auto buildCacheForIndex()
    {
        if constexpr (I == PivotIdx)
        {
            return std::nullptr_t{};
        }
        else
        {
            return NonPivotCache<I>(std::get<I>(mStores));
        }
    }

    template <std::size_t PivotIdx, std::size_t... Is>
    auto buildCachesConst(std::index_sequence<Is...>) const
    {
        return std::make_tuple(
            buildCacheForIndexConst<PivotIdx, Is>()...
        );
    }

    template <std::size_t PivotIdx, std::size_t I>
    auto buildCacheForIndexConst() const
    {
        if constexpr (I == PivotIdx)
        {
            return std::nullptr_t{};
        }
        else
        {
            return NonPivotCacheConst<I>(std::get<I>(mStores));
        }
    }

    // =========================================================================
    // Per-entity combined lookup and invocation
    //
    // invokeFuncIfPresent calls tryGet() once per non-pivot store. If any
    // returns nullptr the entity is skipped. Otherwise func is called with
    // the cached pointer values — no second tryGet() needed.
    //
    // We use a flat "get-then-check" structure rather than fold expressions
    // so each tryGet() result is stored in a local variable and reused for
    // the func call, avoiding redundant sparse lookups.
    // =========================================================================

    template <std::size_t PivotIdx, typename Func, typename PivotT,
              typename Caches, std::size_t... Is>
    void invokeFuncIfPresent(Func&& func, Entity entity, std::size_t denseIdx,
                             PivotT* pivotData, Caches& caches,
                             std::index_sequence<Is...>)
    {
        // Retrieve a typed pointer for each component position.
        // Pivot: address from pre-fetched pivotData array (no lookup).
        // Non-pivot: tryGet() from the pre-cached NonPivotCache (one sparse read).
        auto ptrTuple = std::make_tuple(
            getPtr<PivotIdx, Is>(entity, denseIdx, pivotData, caches)...);

        // Skip entity if any non-pivot pointer is null.
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

    // getPtr<PivotIdx, I>: returns T* for component at index I.
    // For the pivot, returns pivotData[denseIdx]. For others, calls tryGet().
    // Return type is always non-const T* regardless of whether I==PivotIdx,
    // so the tuple element type is uniform within each component position.
    template <std::size_t PivotIdx, std::size_t I, typename PivotT, typename Caches>
    [[nodiscard]] auto* getPtr(Entity entity, std::size_t denseIdx,
                               PivotT* pivotData, Caches& caches) noexcept
    {
        using ElemT = std::tuple_element_t<I, std::tuple<Ts...>>;
        if constexpr (I == PivotIdx)
        {
            return static_cast<ElemT*>(&pivotData[denseIdx]);
        }
        else
        {
            return std::get<I>(caches).tryGet(entity);
        }
    }

    template <std::size_t PivotIdx, std::size_t I, typename PivotT, typename Caches>
    [[nodiscard]] const auto* getPtrConst(Entity entity, std::size_t denseIdx,
                                          const PivotT* pivotData,
                                          const Caches& caches) const noexcept
    {
        using ElemT = std::tuple_element_t<I, std::tuple<Ts...>>;
        if constexpr (I == PivotIdx)
        {
            return static_cast<const ElemT*>(&pivotData[denseIdx]);
        }
        else
        {
            return std::get<I>(caches).tryGet(entity);
        }
    }
};

} // namespace fatp_ecs
