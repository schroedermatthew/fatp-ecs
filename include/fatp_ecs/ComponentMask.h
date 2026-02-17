#pragma once

/**
 * @file ComponentMask.h
 * @brief BitSet-based component masks for archetype matching.
 */

// FAT-P components used:
// - BitSet: Fixed-size bit set with hardware-accelerated popcount/ctz
//
// Each entity can have a bitmask where bit N is set iff the entity has
// the component with TypeId N. Enables O(1) archetype matching
// (isSubsetOf) and scheduler dependency analysis (read/write set
// intersection via BitSet::intersects).
//
// kMaxComponentTypes = 256 is the compile-time ceiling on distinct
// component types (EnTT defaults to 64).

#include <fat_p/BitSet.h>

namespace fatp_ecs
{

/// @brief Maximum number of distinct component types supported.
inline constexpr std::size_t kMaxComponentTypes = 256;

/// @brief A bitmask representing a set of component types.
using ComponentMask = fat_p::BitSet<kMaxComponentTypes>;

// Note: makeComponentMask<Ts...>() is defined in Registry.h after typeId<T>()
// is available, to avoid circular dependency.

} // namespace fatp_ecs
