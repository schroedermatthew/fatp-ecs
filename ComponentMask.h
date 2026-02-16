#pragma once

/**
 * @file ComponentMask.h
 * @brief BitSet-based component masks for fast archetype matching.
 *
 * @details
 * Each entity can have an associated bitmask where bit N is set iff the
 * entity has the component with TypeId N. This enables O(1) archetype
 * matching (isSubsetOf) instead of probing each store individually.
 *
 * Useful for:
 * - Fast archetype matching in queries
 * - Scheduler dependency analysis (read/write sets)
 * - Debug visualization of entity composition
 *
 * FAT-P components used:
 * - BitSet: Fixed-size bit set with hardware-accelerated popcount/ctz
 *
 * @note The maximum number of distinct component types is fixed at
 *       compile time (kMaxComponentTypes = 256). This is generous for
 *       any real game (EnTT defaults to 64).
 */

#include <fat_p/BitSet.h>

namespace fatp_ecs
{

/// @brief Maximum number of distinct component types supported.
static constexpr std::size_t kMaxComponentTypes = 256;

/// @brief A bitmask representing a set of component types.
using ComponentMask = fat_p::BitSet<kMaxComponentTypes>;

// Note: makeComponentMask<Ts...>() is defined in Registry.h after typeId<T>()
// is available, to avoid circular dependency issues.

} // namespace fatp_ecs
