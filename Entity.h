#pragma once

/**
 * @file Entity.h
 * @brief Type-safe entity identifier for the FAT-P ECS framework.
 */

// FAT-P components used:
// - StrongId: Type-safe integer wrapper with tag-based uniqueness
//
// Entity is a 64-bit StrongId packing (index, generation) into a single value.
// Lower 32 bits: slot index (used for SparseSet indexing)
// Upper 32 bits: generation counter (for ABA safety)
//
// Design decisions:
// - UncheckedOpPolicy: Entity IDs are managed internally by the Registry;
//   arithmetic overflow checks on IDs are unnecessary overhead. The SlotMap
//   allocator ensures IDs stay in range.
// - NoCheckPolicy: No per-construction validation needed; IDs are only
//   created by the Registry, never by user code directly.
// - uint64_t: Supports 2^32 concurrent entities with 2^32 generational
//   reuse cycles. Keeps SparseSet's sparse array memory bounded (indexed
//   by the lower 32-bit index only).

#include <cstdint>
#include <functional>
#include <limits>

#include <fat_p/StrongId.h>

namespace fatp_ecs
{

/// @brief Unique tag type distinguishing Entity from other StrongId types.
struct EntityTag
{
};

/// @brief Type-safe entity identifier encoding both slot index and generation.
using Entity = fat_p::StrongId<uint64_t,
                               EntityTag,
                               fat_p::strong_id::NoCheckPolicy,
                               fat_p::strong_id::UncheckedOpPolicy>;

/// @brief Null entity sentinel. Compares unequal to any valid entity.
inline constexpr Entity NullEntity = Entity::invalid();

/**
 * @brief Traits type that bridges Entity to SparseSet and SlotMap.
 *
 * @note Thread-safety: All methods are constexpr and stateless.
 */
struct EntityTraits
{
    using IndexType = uint32_t;
    using GenerationType = uint32_t;

    /// @brief Pack index and generation into an Entity.
    [[nodiscard]] static constexpr Entity make(IndexType index,
                                               GenerationType generation) noexcept
    {
        return Entity(static_cast<uint64_t>(generation) << 32 |
                      static_cast<uint64_t>(index));
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

    /// @brief The maximum valid entity index.
    static constexpr IndexType kMaxIndex = std::numeric_limits<IndexType>::max() - 1;
};

// =============================================================================
// EntityIndex — IndexPolicy for SparseSet/SparseSetWithData
// =============================================================================

/**
 * @brief IndexPolicy that extracts the 32-bit slot index from a 64-bit Entity.
 *
 * This enables SparseSet and SparseSetWithData to key on full Entity values
 * while indexing the sparse array by the 32-bit slot index. Two entities with
 * the same slot index (different generations) map to the same sparse slot,
 * which is the correct semantic: a slot is occupied by at most one live entity.
 */
struct EntityIndex
{
    using sparse_index_type = EntityTraits::IndexType;

    [[nodiscard]] static constexpr sparse_index_type index(
        const Entity& entity) noexcept
    {
        return EntityTraits::index(entity);
    }
};

} // namespace fatp_ecs

// std::hash specialization so Entity can be used in unordered containers.
template <>
struct std::hash<fatp_ecs::Entity>
{
    std::size_t operator()(fatp_ecs::Entity e) const noexcept
    {
        return std::hash<uint64_t>{}(e.get());
    }
};
