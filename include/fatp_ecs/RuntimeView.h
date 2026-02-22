#pragma once

/**
 * @file RuntimeView.h
 * @brief Type-erased runtime view for iterating entities with dynamically
 *        specified component sets.
 */

// FAT-P components used:
// - SmallVector: Inline storage for store pointer arrays (avoids heap
//   allocation for the common case of <= 8 component types per view).
//
// RuntimeView complements the compile-time View<Ts...> for use cases where
// component types are not known until runtime:
//   - Plugin systems that register component types dynamically
//   - Scripting/editor integrations iterating by TypeId string
//   - Generic serialisers and inspectors walking all components on an entity
//   - Debugging tools enumerating arbitrary component combinations
//
// Design: RuntimeView holds SmallVectors of IComponentStore* for include and
// exclude sets. On each() it finds the smallest include store, iterates its
// dense entity array, and probes all other stores via has(). The has() calls
// go through the IComponentStore vtable — this is the inherent cost of runtime
// dispatch and is acceptable for the use cases above, none of which are
// frame-rate-critical inner loops.
//
// Null stores (component type never registered in the Registry) are handled
// the same way as compile-time View:
//   - Null include store => anyStoreNull() => each() is a no-op.
//   - Null exclude store => component never registered => no entity has it
//     => entity is never excluded (exclude check skips null stores).

#include <cstddef>
#include <functional>

#include <fat_p/SmallVector.h>

#include "ComponentStore.h"
#include "Entity.h"
#include "TypeId.h"

namespace fatp_ecs
{

// =============================================================================
// RuntimeView
// =============================================================================

/**
 * @brief Type-erased view over entities matching a runtime-specified set of
 *        component types.
 *
 * Constructed by Registry::runtimeView(). Not intended to be stored across
 * frames — rebuild each time component sets may have changed.
 *
 * @note Thread-safety: NOT thread-safe. Use Scheduler for parallel access.
 *
 * @example
 * @code
 *   // Iterate entities with Position and Velocity, excluding Frozen:
 *   auto view = registry.runtimeView(
 *       {typeId<Position>(), typeId<Velocity>()},
 *       {typeId<Frozen>()});
 *
 *   view.each([](Entity e) {
 *       // process entity e
 *   });
 * @endcode
 */
class RuntimeView
{
public:
    /// @brief Functor type called for each matching entity.
    using EachFn = std::function<void(Entity)>;

    RuntimeView() = default;

    // =========================================================================
    // Construction helpers (called by Registry)
    // =========================================================================

    void addIncludeStore(IComponentStore* store)
    {
        mInclude.push_back(store);
    }

    void addExcludeStore(IComponentStore* store)
    {
        mExclude.push_back(store);
    }

    // =========================================================================
    // Iteration
    // =========================================================================

    /**
     * @brief Call func(Entity) for every entity that has all included
     *        components and none of the excluded components.
     *
     * @param func Callable invoked with each matching entity.
     */
    void each(EachFn func) const
    {
        if (mInclude.empty() || anyIncludeNull())
        {
            return;
        }

        const IComponentStore* pivot = findSmallestStore();
        const Entity*     ents       = pivot->denseEntities();
        const std::size_t cnt        = pivot->denseEntityCount();

        for (std::size_t i = 0; i < cnt; ++i)
        {
            Entity entity = ents[i];

            // All include stores must have this entity.
            if (!allIncludeHave(entity, pivot))
            {
                continue;
            }

            // No exclude store may have this entity.
            if (anyExcludeHas(entity))
            {
                continue;
            }

            func(entity);
        }
    }

    /**
     * @brief Count entities matching the include/exclude criteria.
     */
    [[nodiscard]] std::size_t count() const
    {
        std::size_t result = 0;
        each([&result](Entity) { ++result; });
        return result;
    }

    /**
     * @brief Returns true if there are no matching entities.
     */
    [[nodiscard]] bool empty() const
    {
        if (mInclude.empty() || anyIncludeNull())
        {
            return true;
        }
        // Short-circuit: stop at first match.
        bool found = false;
        each([&found](Entity) { found = true; });
        return !found;
    }

    /**
     * @brief Number of included component types.
     */
    [[nodiscard]] std::size_t includeCount() const noexcept
    {
        return mInclude.size();
    }

    /**
     * @brief Number of excluded component types.
     */
    [[nodiscard]] std::size_t excludeCount() const noexcept
    {
        return mExclude.size();
    }

private:
    // SmallVector<8>: covers the vast majority of real-world views without
    // heap allocation. More than 8 component types in a single view is unusual.
    fat_p::SmallVector<IComponentStore*, 8> mInclude;
    fat_p::SmallVector<IComponentStore*, 8> mExclude;

    // =========================================================================
    // Internal helpers
    // =========================================================================

    [[nodiscard]] bool anyIncludeNull() const noexcept
    {
        for (const auto* store : mInclude)
        {
            if (store == nullptr) return true;
        }
        return false;
    }

    [[nodiscard]] const IComponentStore* findSmallestStore() const noexcept
    {
        const IComponentStore* smallest = mInclude[0];
        for (std::size_t i = 1; i < mInclude.size(); ++i)
        {
            if (mInclude[i] != nullptr && mInclude[i]->size() < smallest->size())
            {
                smallest = mInclude[i];
            }
        }
        return smallest;
    }

    // Returns true if all include stores (other than the pivot) have entity.
    [[nodiscard]] bool allIncludeHave(Entity entity,
                                      const IComponentStore* pivot) const noexcept
    {
        for (const auto* store : mInclude)
        {
            if (store == pivot) continue;
            if (!store->has(entity)) return false;
        }
        return true;
    }

    [[nodiscard]] bool anyExcludeHas(Entity entity) const noexcept
    {
        for (const auto* store : mExclude)
        {
            if (store != nullptr && store->has(entity)) return true;
        }
        return false;
    }
};

} // namespace fatp_ecs
