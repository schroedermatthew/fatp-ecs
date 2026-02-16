#pragma once

/**
 * @file Entity.h
 * @brief Type-safe entity identifier for the FAT-P ECS framework.
 *
 * @details
 * Entity is a lightweight, type-safe ID built on fat_p::StrongId.
 * It wraps a uint32_t and uses a unique tag struct to prevent
 * accidental mixing with other integer-based identifiers.
 *
 * EntityTraits provides the bridge between the type-safe Entity
 * and SparseSet's requirement for an unsigned integral index type.
 *
 * Design decisions:
 * - UncheckedOpPolicy: Entity IDs are managed internally by the
 *   Registry; arithmetic overflow checks on IDs are unnecessary
 *   overhead. The SlotMap allocator ensures IDs stay in range.
 * - NoCheckPolicy: No per-construction validation needed; IDs
 *   are only created by the Registry, never by user code directly.
 * - uint32_t: Supports ~4 billion concurrent entities. Sufficient
 *   for any real-time simulation. Keeps SparseSet's sparse array
 *   memory bounded.
 *
 * @note Thread Safety: Entity itself is a trivial value type (like int).
 *       Thread safety depends on the container holding them.
 */

#include <cstdint>
#include <functional>
#include <limits>

#include <fat_p/StrongId.h>

namespace fatp_ecs
{

// =============================================================================
// Entity Tag
// =============================================================================

/// @brief Unique tag type distinguishing Entity from other StrongId types.
struct EntityTag
{
};

// =============================================================================
// Entity Type
// =============================================================================

/// @brief Type-safe entity identifier encoding both slot index and generation.
/// The lower 32 bits are the slot index, the upper 32 bits are the generation.
using Entity = fat_p::StrongId<uint64_t,
                               EntityTag,
                               fat_p::strong_id::NoCheckPolicy,
                               fat_p::strong_id::UncheckedOpPolicy>;

// =============================================================================
// Entity Constants
// =============================================================================

/// @brief Null entity sentinel. Compares unequal to any valid entity.
inline constexpr Entity NullEntity = Entity::invalid();

// =============================================================================
// EntityTraits
// =============================================================================

/**
 * @brief Traits type that bridges Entity to SparseSet and SlotMap.
 *
 * @details
 * Entity packs (index, generation) into a 64-bit value.
 * - Lower 32 bits: slot index (used for SparseSet indexing)
 * - Upper 32 bits: generation counter (for ABA safety)
 */
struct EntityTraits
{
    /// @brief The underlying index type used by SparseSet.
    using IndexType = uint32_t;
    using GenerationType = uint32_t;

    /// @brief Pack index and generation into an Entity.
    [[nodiscard]] static constexpr Entity make(IndexType index, GenerationType generation) noexcept
    {
        return Entity(static_cast<uint64_t>(generation) << 32 | static_cast<uint64_t>(index));
    }

    /// @brief Extract the slot index from an Entity.
    [[nodiscard]] static constexpr IndexType index(Entity entity) noexcept
    {
        return static_cast<IndexType>(entity.get() & 0xFFFFFFFF);
    }

    /// @brief Extract the generation from an Entity.
    [[nodiscard]] static constexpr GenerationType generation(Entity entity) noexcept
    {
        return static_cast<GenerationType>(entity.get() >> 32);
    }

    /// @brief Convert Entity to the unsigned index SparseSet expects.
    [[nodiscard]] static constexpr IndexType toIndex(Entity entity) noexcept
    {
        return index(entity);
    }

    /// @brief Convert a SparseSet index back to an Entity (generation 0 placeholder).
    [[nodiscard]] static constexpr Entity toEntity(IndexType idx) noexcept
    {
        return Entity(static_cast<uint64_t>(idx));
    }

    /// @brief The maximum valid entity index.
    static constexpr IndexType maxIndex = std::numeric_limits<IndexType>::max() - 1;
};

} // namespace fatp_ecs
