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
// are read from ComponentStore::dense() which provides full 64-bit
// Entity values with generation (via EntityIndex policy), enabling
// safe use of the entities for destroy(), isAlive(), and command
// buffer operations.

#include <algorithm>
#include <cstddef>
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
        const auto& fullEntities = store->dense();
        const std::size_t cnt = store->size();

        for (std::size_t i = 0; i < cnt; ++i)
        {
            func(fullEntities[i], store->dataAt(i));
        }
    }

    template <typename Func>
    void eachSingleComponentConst(Func&& func) const
    {
        const auto* store = std::get<0>(mStores);
        const auto& fullEntities = store->dense();
        const std::size_t cnt = store->size();

        for (std::size_t i = 0; i < cnt; ++i)
        {
            func(fullEntities[i], store->dataAt(i));
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

    template <std::size_t PivotIdx, std::size_t... Is>
    [[nodiscard]] bool entityInAllOthers(Entity entity,
                                         std::index_sequence<Is...>) const
    {
        return ((Is == PivotIdx || std::get<Is>(mStores)->has(entity)) && ...);
    }

    // For the pivot store, the data is at the known dense index — use dataAt()
    // directly instead of re-probing via tryGet(). For all other stores, tryGet()
    // is required since the entity may be at any dense position in those stores.

    template <std::size_t PivotIdx, std::size_t I>
    [[nodiscard]] auto& getComponentAt(Entity entity, std::size_t denseIdx)
    {
        if constexpr (I == PivotIdx)
        {
            return std::get<I>(mStores)->dataAt(denseIdx);
        }
        else
        {
            return *std::get<I>(mStores)->tryGet(entity);
        }
    }

    template <std::size_t PivotIdx, std::size_t I>
    [[nodiscard]] const auto& getComponentAtConst(Entity entity,
                                                  std::size_t denseIdx) const
    {
        if constexpr (I == PivotIdx)
        {
            return std::get<I>(mStores)->dataAt(denseIdx);
        }
        else
        {
            return *std::get<I>(mStores)->tryGet(entity);
        }
    }

    template <typename Func>
    void eachMultiComponent(Func&& func)
    {
        const std::size_t pivotIdx = findSmallestStoreIndex();
        eachWithPivotDispatch(std::forward<Func>(func), pivotIdx,
                              std::index_sequence_for<Ts...>{});
    }

    template <typename Func>
    void eachMultiComponentConst(Func&& func) const
    {
        const std::size_t pivotIdx = findSmallestStoreIndex();
        eachWithPivotDispatchConst(std::forward<Func>(func), pivotIdx,
                                   std::index_sequence_for<Ts...>{});
    }

    template <typename Func, std::size_t... Is>
    void eachWithPivotDispatch(Func&& func, std::size_t pivotIdx,
                               std::index_sequence<Is...>)
    {
        ((pivotIdx == Is
              ? (eachWithPivot<Is>(std::forward<Func>(func)), true)
              : false) ||
         ...);
    }

    template <typename Func, std::size_t... Is>
    void eachWithPivotDispatchConst(Func&& func, std::size_t pivotIdx,
                                    std::index_sequence<Is...>) const
    {
        ((pivotIdx == Is
              ? (eachWithPivotConst<Is>(std::forward<Func>(func)), true)
              : false) ||
         ...);
    }

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivot(Func&& func)
    {
        auto* pivotStore = std::get<PivotIdx>(mStores);
        const auto& fullEntities = pivotStore->dense();
        const std::size_t cnt = pivotStore->size();
        constexpr auto allIndices = std::index_sequence_for<Ts...>{};

        for (std::size_t i = 0; i < cnt; ++i)
        {
            Entity entity = fullEntities[i];

            if (entityInAllOthers<PivotIdx>(entity, allIndices))
            {
                eachWithPivotApply<PivotIdx>(
                    std::forward<Func>(func), entity, i, allIndices);
            }
        }
    }

    template <std::size_t PivotIdx, typename Func, std::size_t... Is>
    void eachWithPivotApply(Func&& func, Entity entity, std::size_t denseIdx,
                            std::index_sequence<Is...>)
    {
        func(entity, getComponentAt<PivotIdx, Is>(entity, denseIdx)...);
    }

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivotConst(Func&& func) const
    {
        const auto* pivotStore = std::get<PivotIdx>(mStores);
        const auto& fullEntities = pivotStore->dense();
        const std::size_t cnt = pivotStore->size();
        constexpr auto allIndices = std::index_sequence_for<Ts...>{};

        for (std::size_t i = 0; i < cnt; ++i)
        {
            Entity entity = fullEntities[i];

            if (entityInAllOthers<PivotIdx>(entity, allIndices))
            {
                eachWithPivotConstApply<PivotIdx>(
                    std::forward<Func>(func), entity, i, allIndices);
            }
        }
    }

    template <std::size_t PivotIdx, typename Func, std::size_t... Is>
    void eachWithPivotConstApply(Func&& func, Entity entity,
                                 std::size_t denseIdx,
                                 std::index_sequence<Is...>) const
    {
        func(entity, getComponentAtConst<PivotIdx, Is>(entity, denseIdx)...);
    }
};

} // namespace fatp_ecs
