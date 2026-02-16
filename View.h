#pragma once

/**
 * @file View.h
 * @brief Component view for iterating entities with specific component sets.
 *
 * @details
 * View<Ts...> provides a way to iterate over all entities that possess
 * every component type in Ts. It does NOT copy or cache entity lists;
 * it iterates on-the-fly by walking the smallest ComponentStore and
 * probing the others for each entity.
 *
 * This is the standard sparse-set ECS iteration strategy (same as EnTT):
 * - Find the store with the fewest entities (the "pivot").
 * - For each entity in the pivot, check contains() on all other stores.
 * - Only yield entities present in ALL stores.
 *
 * The O(1) contains() of SparseSet makes the probe step fast.
 * Iterating the smallest set minimizes the total number of probes.
 *
 * Single-component views skip the intersection entirely and iterate
 * the dense array directly — no probing needed.
 *
 * @note Thread Safety: NOT thread-safe during iteration if any store
 *       is being modified concurrently.
 */

#include <algorithm>
#include <cstddef>
#include <tuple>

#include "ComponentStore.h"
#include "Entity.h"

namespace fatp_ecs
{

// Forward declaration
class Registry;

// =============================================================================
// View - Multi-Component Iteration
// =============================================================================

/**
 * @brief Iterates entities possessing all component types in Ts.
 *
 * @tparam Ts Component types to require.
 *
 * @details
 * View does not own or copy data. It holds pointers to ComponentStores
 * owned by the Registry. The View is lightweight and intended to be
 * created on-the-fly per iteration, not stored long-term.
 *
 * Usage:
 * @code
 * auto view = registry.view<Position, Velocity>();
 * view.each([](Entity e, Position& pos, Velocity& vel) {
 *     pos.x += vel.dx;
 * });
 * @endcode
 */
template <typename... Ts>
class View
{
    static_assert(sizeof...(Ts) > 0, "View requires at least one component type");

public:
    /**
     * @brief Constructs a View over the given component stores.
     *
     * @param stores Pointers to ComponentStore<T> for each T in Ts.
     *               Any nullptr means no entities will match.
     */
    explicit View(ComponentStore<Ts>*... stores)
        : mStores(stores...)
    {
    }

    // =========================================================================
    // Iteration
    // =========================================================================

    /**
     * @brief Calls func(Entity, T&...) for each entity with all components.
     *
     * @tparam Func Callable with signature void(Entity, Ts&...).
     * @param func The function to invoke per matching entity.
     *
     * @details
     * For single-component views, iterates the dense array directly.
     * For multi-component views, iterates the smallest store and probes
     * the others.
     *
     * @note The callback receives mutable references to components.
     *       Modifying component values during iteration is safe.
     *       Adding or removing components during iteration is NOT safe.
     */
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

    /**
     * @brief Calls func(Entity, const T&...) for each matching entity.
     *
     * @tparam Func Callable with signature void(Entity, const Ts&...).
     * @param func The function to invoke per matching entity.
     */
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

    /**
     * @brief Returns the number of entities that match this view.
     *
     * @details
     * For single-component views, this is exact (O(1)).
     * For multi-component views, this requires full iteration (O(n*k)
     * where n is the smallest store size and k is the number of stores).
     * Prefer checking empty() or using each() directly.
     */
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
    // Single-component iteration (no intersection needed)
    // =========================================================================

    template <typename Func>
    void eachSingleComponent(Func&& func)
    {
        auto* store = std::get<0>(mStores);
        const auto& denseEntities = store->dense();
        const std::size_t count = store->size();

        for (std::size_t i = 0; i < count; ++i)
        {
            Entity entity = EntityTraits::toEntity(denseEntities[i]);
            func(entity, store->dataAt(i));
        }
    }

    template <typename Func>
    void eachSingleComponentConst(Func&& func) const
    {
        const auto* store = std::get<0>(mStores);
        const auto& denseEntities = store->dense();
        const std::size_t count = store->size();

        for (std::size_t i = 0; i < count; ++i)
        {
            Entity entity = EntityTraits::toEntity(denseEntities[i]);
            func(entity, store->dataAt(i));
        }
    }

    // =========================================================================
    // Multi-component iteration (smallest-set intersection)
    // =========================================================================

    /**
     * @brief Finds which store index has the fewest entities.
     */
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

    /**
     * @brief Checks if entity exists in all stores except the pivot.
     */
    template <std::size_t PivotIdx, std::size_t... Is>
    [[nodiscard]] bool entityInAllOthers(Entity entity, std::index_sequence<Is...>) const
    {
        return ((Is == PivotIdx || std::get<Is>(mStores)->has(entity)) && ...);
    }

    /**
     * @brief Gets component references for an entity from all stores.
     */
    template <std::size_t... Is>
    [[nodiscard]] auto getComponents(Entity entity, std::index_sequence<Is...>)
    {
        return std::forward_as_tuple(*std::get<Is>(mStores)->tryGet(entity)...);
    }

    template <std::size_t... Is>
    [[nodiscard]] auto getComponentsConst(Entity entity, std::index_sequence<Is...>) const
    {
        return std::forward_as_tuple(
            static_cast<const typename std::tuple_element_t<Is, std::tuple<ComponentStore<Ts>*...>>
                             ::element_type::DataType&>(
                *std::get<Is>(mStores)->tryGet(entity))...);
    }

    /**
     * @brief Iterates using a specific store index as the pivot.
     *
     * @details
     * We use a compile-time dispatch to iterate over each possible
     * pivot index. At runtime, only the one matching the smallest
     * store actually executes. This avoids virtual dispatch or
     * function pointer indirection.
     */
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
        // Expand a fold that dispatches to the correct pivot at runtime.
        ((pivotIdx == Is ? (eachWithPivot<Is>(std::forward<Func>(func)), true) : false) || ...);
    }

    template <typename Func, std::size_t... Is>
    void eachWithPivotDispatchConst(Func&& func, std::size_t pivotIdx,
                                    std::index_sequence<Is...>) const
    {
        ((pivotIdx == Is ? (eachWithPivotConst<Is>(std::forward<Func>(func)), true) : false) || ...);
    }

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivot(Func&& func)
    {
        auto* pivotStore = std::get<PivotIdx>(mStores);
        const auto& denseEntities = pivotStore->dense();
        const std::size_t count = pivotStore->size();
        constexpr auto allIndices = std::index_sequence_for<Ts...>{};

        for (std::size_t i = 0; i < count; ++i)
        {
            Entity entity = EntityTraits::toEntity(denseEntities[i]);

            if (entityInAllOthers<PivotIdx>(entity, allIndices))
            {
                auto components = getComponents(entity, allIndices);
                std::apply([&](auto&... comps) { func(entity, comps...); }, components);
            }
        }
    }

    template <std::size_t PivotIdx, typename Func>
    void eachWithPivotConst(Func&& func) const
    {
        const auto* pivotStore = std::get<PivotIdx>(mStores);
        const auto& denseEntities = pivotStore->dense();
        const std::size_t count = pivotStore->size();
        constexpr auto allIndices = std::index_sequence_for<Ts...>{};

        for (std::size_t i = 0; i < count; ++i)
        {
            Entity entity = EntityTraits::toEntity(denseEntities[i]);

            if (entityInAllOthers<PivotIdx>(entity, allIndices))
            {
                // Use tryGet from each store and cast to const
                std::apply(
                    [&](auto*... ptrs) { func(entity, static_cast<const Ts&>(*ptrs)...); },
                    std::make_tuple(std::get<ComponentStore<Ts>*>(mStores)->tryGet(entity)...));
            }
        }
    }
};

} // namespace fatp_ecs
